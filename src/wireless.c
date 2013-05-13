//
//  Connecting to a Vita PTP/IP device
//  VitaMTP
//
//  Created by Yifan Lu
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ptp.h"
#include "vitamtp.h"

#define REQUEST_BUFFER_SIZE 100
#define RESPONSE_MAX_SIZE 100

struct vita_device
{
    PTPParams *params;
    enum vita_device_type device_type;
    char guid[33];
    struct vita_network {
        int registered;
        int init_sockfd;
        struct sockaddr_in addr;
        int data_port;
    } network_device;
};

extern int g_VitaMTP_logmask;
static int g_stopbroadcast;

void VitaMTP_hex_dump(const unsigned char *data, unsigned int size, unsigned int num);

// the code below is taken from gphoto2
/* ptpip.c
 *
 * Copyright (C) 2006 Marcus Meissner <marcus@jet.franken.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/*
 * This is working, but unfinished!
 * - Event handling not finished.
 * - Some configure checking magic missing for the special header files
 *   and functions.
 * - Not everything implementation correctly cross checked.
 * - Coolpix P3 does not give transfer status (image 000x/000y), and reports an
 *   error when transfers finish correctly.
 *
 * Nikon WU-1* adapters might use 0011223344556677 as GUID always...
 */

#define PTPIP_VERSION_MAJOR 0x0001
#define PTPIP_VERSION_MINOR 0x0000

#include "gphoto2-endian.h"
#include "ptp-pack.c"

#define ptpip_len		0
#define ptpip_type		4

#define ptpip_cmd_dataphase	8
#define ptpip_cmd_code		12
#define ptpip_cmd_transid	14
#define ptpip_cmd_param1	18
#define ptpip_cmd_param2	22
#define ptpip_cmd_param3	26
#define ptpip_cmd_param4	30
#define ptpip_cmd_param5	34

static uint16_t ptp_ptpip_check_event (PTPParams* params);

/* send / receive functions */
uint16_t
ptp_ptpip_sendreq (PTPParams* params, PTPContainer* req)
{
	ssize_t 		ret;
	uint32_t		len = 18+req->Nparam*4;
	unsigned char 		*request = malloc(len);
    
	//ptp_ptpip_check_event (params);
    
	htod32a(&request[ptpip_type],PTPIP_CMD_REQUEST);
	htod32a(&request[ptpip_len],len);
	htod32a(&request[ptpip_cmd_dataphase],1);	/* FIXME: dataphase handling */
	htod16a(&request[ptpip_cmd_code],req->Code);
	htod32a(&request[ptpip_cmd_transid],req->Transaction_ID);
    
	switch (req->Nparam) {
        case 5: htod32a(&request[ptpip_cmd_param5],req->Param5);
        case 4: htod32a(&request[ptpip_cmd_param4],req->Param4);
        case 3: htod32a(&request[ptpip_cmd_param3],req->Param3);
        case 2: htod32a(&request[ptpip_cmd_param2],req->Param2);
        case 1: htod32a(&request[ptpip_cmd_param1],req->Param1);
        case 0:
        default:
            break;
	}
    VitaMTP_Log(VitaMTP_DEBUG, "ptpip/oprequest\n");
    if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
        VitaMTP_hex_dump(request, len, 16);
    }
	ret = write (params->cmdfd, request, len);
	free (request);
	if (ret == -1)
		perror ("sendreq/write to cmdfd");
	if (ret != len) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip: ptp_ptpip_sendreq() len =%d but ret=%zd\n", len, ret);
		return PTP_RC_OK;
	}
	return PTP_RC_OK;
}

static uint16_t
ptp_ptpip_generic_read (PTPParams *params, int fd, PTPIPHeader *hdr, unsigned char**data) {
	ssize_t ret;
    int	len, curread;
	unsigned char *xhdr;
    
	xhdr = (unsigned char*)hdr; curread = 0; len = sizeof (PTPIPHeader);
	while (curread < len) {
		ret = read (fd, xhdr + curread, len - curread);
		if (ret == -1) {
			perror ("read PTPIPHeader");
			return PTP_RC_GeneralError;
		}
        VitaMTP_Log(VitaMTP_DEBUG, "ptpip/generic_read\n");
        if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
            VitaMTP_hex_dump(xhdr+curread, (unsigned int)ret, 16);
        }
		curread += ret;
		if (ret == 0) {
			VitaMTP_Log(VitaMTP_ERROR, "ptpip: End of stream after reading %zd bytes of ptpipheader\n", ret);
			return PTP_RC_GeneralError;
		}
	}
	len = dtoh32 (hdr->length) - sizeof (PTPIPHeader);
	if (len < 0) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/generic_read: len < 0, %d?\n", len);
		return PTP_RC_GeneralError;
	}
	*data = malloc (len);
	if (!*data) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/generic_read: malloc failed.\n");
		return PTP_RC_GeneralError;
	}
	curread = 0;
	while (curread < len) {
		ret = read (fd, (*data)+curread, len-curread);
		if (ret == -1) {
			VitaMTP_Log(VitaMTP_ERROR, "ptpip/generic_read: error %d in reading PTPIP data\n", errno);
			free (*data);*data = NULL;
			return PTP_RC_GeneralError;
		} else {
            VitaMTP_Log(VitaMTP_DEBUG, "ptpip/generic_read\n");
            if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
                VitaMTP_hex_dump(((*data)+curread), (unsigned int)ret, 16);
            }
		}
		if (ret == 0)
			break;
		curread += ret;
	}
	if (curread != len) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/generic_read: read PTPIP data, ret %zd vs len %d\n", ret, len);
		free (*data);*data = NULL;
		return PTP_RC_GeneralError;
	}
	return PTP_RC_OK;
}

static uint16_t
ptp_ptpip_cmd_read (PTPParams* params, PTPIPHeader *hdr, unsigned char** data) {
	//ptp_ptpip_check_event (params);
	return ptp_ptpip_generic_read (params, params->cmdfd, hdr, data);
}

static uint16_t
ptp_ptpip_evt_read (PTPParams* params, PTPIPHeader *hdr, unsigned char** data) {
	return ptp_ptpip_generic_read (params, params->evtfd, hdr, data);
}

#define ptpip_startdata_transid		0
#define ptpip_startdata_totallen	4
#define ptpip_startdata_unknown		8
#define ptpip_data_transid		0
#define ptpip_data_payload		4

#define WRITE_BLOCKSIZE 65536
uint16_t
ptp_ptpip_senddata (PTPParams* params, PTPContainer* ptp,
                    unsigned long size, PTPDataHandler *handler
                    ) {
	unsigned char	request[0x14];
    ssize_t ret;
	int		curwrite, towrite;
	unsigned char*	xdata;
    
	htod32a(&request[ptpip_type],PTPIP_START_DATA_PACKET);
	htod32a(&request[ptpip_len],sizeof(request));
	htod32a(&request[ptpip_startdata_transid  + 8],ptp->Transaction_ID);
	htod32a(&request[ptpip_startdata_totallen + 8],(uint32_t)size);
	htod32a(&request[ptpip_startdata_unknown  + 8],0);
    VitaMTP_Log(VitaMTP_DEBUG, "ptpip/senddata\n");
    if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
        VitaMTP_hex_dump(request, sizeof(request), 16);
    }
	ret = write (params->cmdfd, request, sizeof(request));
	if (ret == -1)
		perror ("sendreq/write to cmdfd");
	if (ret != sizeof(request)) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/senddata: ptp_ptpip_senddata() len=%d but ret=%zd\n", (int)sizeof(request), ret);
		return PTP_RC_GeneralError;
	}
	xdata = malloc(WRITE_BLOCKSIZE+8+4);
	if (!xdata) return PTP_RC_GeneralError;
	curwrite = 0;
	while (curwrite < size) {
		unsigned long type, written, towrite2, xtowrite;
        
		//ptp_ptpip_check_event (params);
        
		towrite = (int)(size - curwrite);
		if (towrite > WRITE_BLOCKSIZE) {
			towrite	= WRITE_BLOCKSIZE;
			type	= PTPIP_DATA_PACKET;
		} else {
			type	= PTPIP_END_DATA_PACKET;
		}
		ret = handler->getfunc (params, handler->priv, towrite, &xdata[ptpip_data_payload+8], &xtowrite);
		if (ret == -1) {
			perror ("getfunc in senddata failed");
			free (xdata);
			return PTP_RC_GeneralError;
		}
		towrite2 = xtowrite + 12;
		htod32a(&xdata[ptpip_type], (uint32_t)type);
		htod32a(&xdata[ptpip_len], (uint32_t)towrite2);
		htod32a(&xdata[ptpip_data_transid+8], ptp->Transaction_ID);
        VitaMTP_Log(VitaMTP_DEBUG, "ptpip/senddata\n");
        if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
            VitaMTP_hex_dump(xdata, (unsigned int)towrite2, 16);
        }
		written = 0;
		while (written < towrite2) {
			ret = write (params->cmdfd, xdata+written, towrite2-written);
			if (ret == -1) {
				perror ("write in senddata failed");
				free (xdata);
				return PTP_RC_GeneralError;
			}
			written += ret;
		}
		curwrite += towrite;
	}
	free (xdata);
	return PTP_RC_OK;
}

uint16_t
ptp_ptpip_getdata (PTPParams* params, PTPContainer* ptp, PTPDataHandler *handler) {
	PTPIPHeader		hdr;
	unsigned char		*xdata = NULL;
	uint16_t 		ret;
	unsigned long		toread, curread;
	int			xret;
    
	ret = ptp_ptpip_cmd_read (params, &hdr, &xdata);
	if (ret != PTP_RC_OK)
		return ret;
    
	if (dtoh32(hdr.type) == PTPIP_CMD_RESPONSE) { /* might happen if we have no data transfer due to error? */
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: Unexpected ptp response, code %x\n", dtoh32a(&xdata[8]));
		return PTP_RC_GeneralError;
	}
	if (dtoh32(hdr.type) != PTPIP_START_DATA_PACKET) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: got reply type %d\n", dtoh32(hdr.type));
		return PTP_RC_GeneralError;
	}
	toread = dtoh32a(&xdata[ptpip_data_payload]);
	free (xdata); xdata = NULL;
	curread = 0;
	while (curread < toread) {
		ret = ptp_ptpip_cmd_read (params, &hdr, &xdata);
		if (ret != PTP_RC_OK)
			return ret;
		if (dtoh32(hdr.type) == PTPIP_END_DATA_PACKET) {
			unsigned long written;
			unsigned long datalen = dtoh32(hdr.length)-8-ptpip_data_payload;
			if (datalen > (toread-curread)) {
				VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: returned data is too much, expected %ld, got %ld\n",
                        (toread-curread),datalen
                        );
				break;
			}
			xret = handler->putfunc (params, handler->priv,
                                     datalen, xdata+ptpip_data_payload, &written
                                     );
			if (xret == -1) {
				VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: failed to putfunc of returned data\n");
				break;
			}
			curread += written;
			free (xdata); xdata = NULL;
			continue;
		}
		if (dtoh32(hdr.type) == PTPIP_DATA_PACKET) {
			unsigned long written;
			unsigned long datalen = dtoh32(hdr.length)-8-ptpip_data_payload;
			if (datalen > (toread-curread)) {
				VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: returned data is too much, expected %ld, got %ld\n",
                        (toread-curread),datalen
                        );
				break;
			}
			xret = handler->putfunc (params, handler->priv,
                                     datalen, xdata+ptpip_data_payload, &written
                                     );
			if (xret == -1) {
				VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: failed to putfunc of returned data\n");
				break;
			}
			curread += written;
			free (xdata); xdata = NULL;
			continue;
		}
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/getdata: ret type %d\n", hdr.type);
	}
	if (curread < toread)
		return PTP_RC_GeneralError;
	return PTP_RC_OK;
}

#define ptpip_resp_code		0
#define ptpip_resp_transid	2
#define ptpip_resp_param1	6
#define ptpip_resp_param2	10
#define ptpip_resp_param3	14
#define ptpip_resp_param4	18
#define ptpip_resp_param5	22

uint16_t
ptp_ptpip_getresp (PTPParams* params, PTPContainer* resp)
{
	PTPIPHeader	hdr;
	unsigned char	*data = NULL;
	uint16_t 	ret;
	int		n;
    
	ret = ptp_ptpip_cmd_read (params, &hdr, &data);
	if (ret != PTP_RC_OK)
		return ret;
    
	resp->Code		= dtoh16a(&data[ptpip_resp_code]);
	resp->Transaction_ID	= dtoh32a(&data[ptpip_resp_transid]);
	n = (dtoh32(hdr.length) - ptpip_resp_param1)/sizeof(uint32_t);
	switch (n) {
        case 5: resp->Param5 = dtoh32a(&data[ptpip_resp_param5]);
        case 4: resp->Param4 = dtoh32a(&data[ptpip_resp_param4]);
        case 3: resp->Param3 = dtoh32a(&data[ptpip_resp_param3]);
        case 2: resp->Param2 = dtoh32a(&data[ptpip_resp_param2]);
        case 1: resp->Param1 = dtoh32a(&data[ptpip_resp_param1]);
        case 0: break;
        default:
            VitaMTP_Log(VitaMTP_ERROR, "ptpip/getresp: response got %d parameters?\n", n);
            break;
	}
	free (data);
	return PTP_RC_OK;
}

#define ptpip_initcmd_guid	8
#define ptpip_initcmd_name	24

static uint16_t
ptp_ptpip_init_command_request (PTPParams* params)
{
	char		hostname[100];
	unsigned char*	cmdrequest;
	unsigned int 		len,i;
    ssize_t ret;
	unsigned char	guid[16];
	
	if (gethostname (hostname, sizeof(hostname)))
		return PTP_RC_GeneralError;
	len = (unsigned int)(ptpip_initcmd_name + (strlen(hostname)+1)*2 + 4);
    
	cmdrequest = malloc(len);
	htod32a(&cmdrequest[ptpip_type],PTPIP_INIT_COMMAND_REQUEST);
	htod32a(&cmdrequest[ptpip_len],len);
    
	memcpy(&cmdrequest[ptpip_initcmd_guid], guid, 16);
	for (i=0;i<strlen(hostname)+1;i++) {
		/* -> ucs-2 in little endian */
		cmdrequest[ptpip_initcmd_name+i*2] = hostname[i];
		cmdrequest[ptpip_initcmd_name+i*2+1] = 0;
	}
	htod16a(&cmdrequest[ptpip_initcmd_name+(strlen(hostname)+1)*2],PTPIP_VERSION_MAJOR);
	htod16a(&cmdrequest[ptpip_initcmd_name+(strlen(hostname)+1)*2+2],PTPIP_VERSION_MINOR);
    
    VitaMTP_Log(VitaMTP_DEBUG, "ptpip/init_cmd recieved\n");
    if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
        VitaMTP_hex_dump(cmdrequest, len, 16);
    }
	ret = write (params->cmdfd, cmdrequest, len);
	free (cmdrequest);
	if (ret == -1) {
		perror("write init cmd request");
		return PTP_RC_GeneralError;
	}
	VitaMTP_Log(VitaMTP_ERROR, "ptpip/init_cmd: return %zd / len %d\n", ret, len);
	if (ret != len) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip: return %zd vs len %d\n", ret, len);
		return PTP_RC_GeneralError;
	}
	return PTP_RC_OK;
}

#define ptpip_cmdack_idx	0
#define ptpip_cmdack_guid	4
#define ptpip_cmdack_name	20

static uint16_t
ptp_ptpip_init_command_ack (PTPParams* params)
{
	PTPIPHeader	hdr;
	unsigned char	*data = NULL;
	uint16_t 	ret;
	int		i;
	unsigned short	*name;
    
	ret = ptp_ptpip_generic_read (params, params->cmdfd, &hdr, &data);
	if (ret != PTP_RC_OK)
		return ret;
	if (hdr.type != dtoh32(PTPIP_INIT_COMMAND_ACK)) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/init_cmd_ack: bad type returned %d\n", htod32(hdr.type));
		return PTP_RC_GeneralError;
	}
	params->eventpipeid = dtoh32a(&data[ptpip_cmdack_idx]);
	memcpy (params->cameraguid, &data[ptpip_cmdack_guid], 16);
	name = (unsigned short*)&data[ptpip_cmdack_name];
	for (i=0;name[i];i++) /* EMPTY */;
	params->cameraname = malloc((i+1)*sizeof(uint16_t));
	for (i=0;name[i];i++)
		params->cameraname[i] = name[i];
	free (data);
	return PTP_RC_OK;
}

#define ptpip_eventinit_idx	8
#define ptpip_eventinit_size	12
static uint16_t
ptp_ptpip_init_event_request (PTPParams* params)
{
	unsigned char	evtrequest[ptpip_eventinit_size];
	ssize_t 		ret;
    
	htod32a(&evtrequest[ptpip_type],PTPIP_INIT_EVENT_REQUEST);
	htod32a(&evtrequest[ptpip_len],ptpip_eventinit_size);
	htod32a(&evtrequest[ptpip_eventinit_idx],params->eventpipeid);
    
	VitaMTP_Log(VitaMTP_DEBUG, "ptpip/init_event recieved\n");
    if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
        VitaMTP_hex_dump(evtrequest, ptpip_eventinit_size, 16);
    }
	ret = write (params->evtfd, evtrequest, ptpip_eventinit_size);
	if (ret == -1) {
		perror("write init evt request");
		return PTP_RC_GeneralError;
	}
	if (ret != ptpip_eventinit_size) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip: unexpected retsize %zd, expected %d\n", ret, ptpip_eventinit_size);
		return PTP_RC_GeneralError;
	}
	return PTP_RC_OK;
}

static uint16_t
ptp_ptpip_init_event_ack (PTPParams* params)
{
	PTPIPHeader	hdr;
	unsigned char	*data = NULL;
	uint16_t	ret;
    
	ret = ptp_ptpip_evt_read (params, &hdr, &data);
	if (ret != PTP_RC_OK)
		return ret;
	if (hdr.type != dtoh32(PTPIP_INIT_EVENT_ACK)) {
		VitaMTP_Log(VitaMTP_ERROR, "ptpip: bad type returned %d\n", htod32(hdr.type));
		return PTP_RC_GeneralError;
	}
	free (data);
	return PTP_RC_OK;
}


/* Event handling functions */

/* PTP Events wait for or check mode */
#define PTP_EVENT_CHECK			0x0000	/* waits for */
#define PTP_EVENT_CHECK_FAST		0x0001	/* checks */

#define ptpip_event_code    0
#define ptpip_event_transid	2
#define ptpip_event_param1	6
#define ptpip_event_param2	10
#define ptpip_event_param3	14
static inline uint16_t
ptp_ptpip_event (PTPParams* params, PTPContainer* event, int wait)
{
	fd_set		infds;
	struct timeval	timeout;
	int ret;
	unsigned char*	data = NULL;
	PTPIPHeader	hdr;
    int n;
    
	while (1) {
		if (wait == PTP_EVENT_CHECK_FAST) {
			FD_ZERO(&infds);
			FD_SET(params->evtfd, &infds);
			timeout.tv_sec = 0;
			timeout.tv_usec = 1;
			if (1 != select (params->evtfd+1, &infds, NULL, NULL, &timeout))
				return PTP_RC_OK;
		}
		ret = ptp_ptpip_evt_read (params, &hdr, &data);
		if (ret != PTP_RC_OK)
			return ret;
		VitaMTP_Log(VitaMTP_DEBUG,"ptpip/event: hdr type %d, length %d\n", hdr.type, hdr.length);
        
		if (dtoh32(hdr.type) == PTPIP_EVENT) {
			break;
		}
        
		// TODO: Handle cancel transaction and ping/pong
		VitaMTP_Log(VitaMTP_ERROR, "ptpip/event: unknown/unhandled event type %d\n", hdr.type);
	}
    
	event->Code		= dtoh16a(&data[ptpip_event_code]);
	event->Transaction_ID	= dtoh32a(&data[ptpip_event_transid]);
	n = (dtoh32(hdr.length) - ptpip_event_param1)/sizeof(uint32_t);
	switch (n) {
        case 3: event->Param3 = dtoh32a(&data[ptpip_event_param3]);
        case 2: event->Param2 = dtoh32a(&data[ptpip_event_param2]);
        case 1: event->Param1 = dtoh32a(&data[ptpip_event_param1]);
        case 0: break;
        default:
            VitaMTP_Log(VitaMTP_ERROR, "ptpip/event: response got %d parameters?\n", n);
            break;
	}
	free (data);
	return PTP_RC_OK;
}

uint16_t
ptp_ptpip_event_check (PTPParams* params, PTPContainer* event) {
	return ptp_ptpip_event (params, event, PTP_EVENT_CHECK_FAST);
}

uint16_t
ptp_ptpip_event_wait (PTPParams* params, PTPContainer* event) {
	return ptp_ptpip_event (params, event, PTP_EVENT_CHECK);
}

static int
VitaMTP_PTPIP_Connect (PTPParams* params, struct sockaddr_in *saddr, int port) {
	uint16_t	ret;
    
	VitaMTP_Log(VitaMTP_DEBUG, "ptpip/connect: connecting to port %d.\n", port);
	saddr->sin_port		= htons(port);
	params->cmdfd = socket (PF_INET, SOCK_STREAM, 0);
	if (params->cmdfd == -1) {
		perror ("socket cmd");
		return -1;
	}
	params->evtfd = socket (PF_INET, SOCK_STREAM, 0);
	if (params->evtfd == -1) {
		perror ("socket evt");
		close (params->cmdfd);
		return -1;
	}
	if (-1 == connect (params->cmdfd, (struct sockaddr*)saddr, sizeof(struct sockaddr_in))) {
		perror ("connect cmd");
		close (params->cmdfd);
		close (params->evtfd);
		return -1;
	}
	ret = ptp_ptpip_init_command_request (params);
	if (ret != PTP_RC_OK)
		return -1;
	ret = ptp_ptpip_init_command_ack (params);
	if (ret != PTP_RC_OK)
		return -1;
	if (-1 == connect (params->evtfd, (struct sockaddr*)&saddr, sizeof(struct sockaddr_in))) {
		perror ("connect evt");
		close (params->cmdfd);
		close (params->evtfd);
		return -1;
	}
	ret = ptp_ptpip_init_event_request (params);
	if (ret != PTP_RC_OK)
		return -1;
	ret = ptp_ptpip_init_event_ack (params);
	if (ret != PTP_RC_OK)
		return -1;
	VitaMTP_Log(VitaMTP_DEBUG, "ptpip/connect: ptpip connected!\n");
	return -1;
}

// end code from ptpip.c

static int VitaMTP_Data_Connect(vita_device_t *device) {
    device->params = malloc(sizeof(PTPParams));
    if (device->params == NULL) {
        VitaMTP_Log(VitaMTP_DEBUG, "out of memory\n");
        return -1;
    }
    memset(device->params, 0, sizeof(PTPParams));
    if (VitaMTP_PTPIP_Connect(device->params, &device->network_device.addr, device->network_device.data_port) < 0) {
        VitaMTP_Log(VitaMTP_DEBUG, "cannot connect to PTP/IP protocol\n");
        free(device->params);
        return -1;
    }
    device->params->sendreq_func	= ptp_ptpip_sendreq;
    device->params->senddata_func	= ptp_ptpip_senddata;
    device->params->getresp_func	= ptp_ptpip_getresp;
    device->params->getdata_func	= ptp_ptpip_getdata;
    device->params->event_wait	= ptp_ptpip_event_wait;
    device->params->event_check	= ptp_ptpip_event_check;
    return 0;
}

static int VitaMTP_Sock_Read_All(int sockfd, unsigned char **p_data, size_t *p_len, struct sockaddr *src_addr, socklen_t *addrlen) {
    unsigned char buffer[REQUEST_BUFFER_SIZE];
    unsigned char *data = NULL;
    ssize_t len = 0;
    while (1) {
        ssize_t clen;
        if ((clen = recvfrom(sockfd, buffer, REQUEST_BUFFER_SIZE, len > 0 ? MSG_DONTWAIT : 0, src_addr, addrlen)) < 0) {
            if (errno == EWOULDBLOCK) {
                break;
            }
            VitaMTP_Log(VitaMTP_ERROR, "error recieving data\n");
            free(data);
            return -1;
        }
        if (clen == 0) {
            break;
        }
        VitaMTP_Log(VitaMTP_DEBUG, "Recieved %d bytes from socket %d\n", (unsigned int)clen, sockfd);
        if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
            VitaMTP_hex_dump(buffer, (unsigned int)clen, 16);
        }
        data = realloc(data, len+clen);
        memcpy(data+len, buffer, clen);
        len += clen;
    }
    *p_data = data;
    *p_len = len;
    return 0;
}

static int VitaMTP_Sock_Write_All(int sockfd, const unsigned char *data, size_t len, const struct sockaddr *dest_addr, socklen_t addrlen) {
    while (1) {
        ssize_t clen;
        if ((clen = sendto(sockfd, data, len, 0, dest_addr, addrlen)) == len) {
            break;
        }
        if (clen < 0) {
            return -1;
        }
        data += clen;
        len -= clen;
    }
    VitaMTP_Log(VitaMTP_DEBUG, "Sent %d bytes to socket %d\n", (unsigned int)len, sockfd);
    if (MASK_SET(g_VitaMTP_logmask, VitaMTP_DEBUG)) {
        VitaMTP_hex_dump(data, (unsigned int)len, 16);
    }
    return 0;
}

int VitaMTP_Broadcast_Host(wireless_host_info_t *info, unsigned int host_addr) {
    char *host_response;
    if (asprintf(&host_response, "HTTP/1.1 200 OK\r\nhost-id:%s\r\nhost-type:%s\r\nhost-name:%s\r\nhost-mtp-protocol-version:%08d\r\nhost-request-port:%d\r\nhost-wireless-protocol-version:%08d\r\n", info->guid, info->type, info->name, VITAMTP_PROTOCOL_MAX_VERSION, info->port, VITAMTP_WIRELESS_MAX_VERSION) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "out of memory\n");
        free(host_response);
        return -1;
    }
    
    int sock;
    struct sockaddr_in si_host;
    struct sockaddr_in si_client;
    unsigned int slen = sizeof(si_client);
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot create broadcast socket\n");
        free(host_response);
        return -1;
    }
    memset(&si_host, 0, sizeof(si_host));
    si_host.sin_family = AF_INET;
    si_host.sin_port = htons(info->port);
    si_host.sin_addr.s_addr = host_addr ? htonl(host_addr) : htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&si_host, sizeof(si_host)) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot bind listening socket\n");
        free(host_response);
        return -1;
    }
    
    char *data;
    size_t len;
    g_stopbroadcast = 0;
    VitaMTP_Log(VitaMTP_DEBUG, "start broadcasting as: %s\n", info->name);
    while (!g_stopbroadcast) {
        if (VitaMTP_Sock_Read_All(sock, (unsigned char **)&data, &len, (struct sockaddr *)&si_client, &slen) < 0) {
            VitaMTP_Log(VitaMTP_ERROR, "error recieving data\n");
            free(host_response);
            close(sock);
            return -1;
        }
        if (len == 0) {
            //VitaMTP_Log(VitaMTP_DEBUG, "No clients found.\n");
            sleep(1);
            continue;
        }
        if (strcmp(data, "SRCH * HTTP/1.1\r\n")) {
            VitaMTP_Log(VitaMTP_DEBUG, "Unknown request: %.*s\n", (int)len, data);
            free(data);
            continue;
        }
        if (VitaMTP_Sock_Write_All(sock, (unsigned char *)host_response, strlen(host_response)+1, (struct sockaddr *)&si_client, slen) < 0) {
            VitaMTP_Log(VitaMTP_ERROR, "error sending response\n");
            free(host_response);
            free(data);
            close(sock);
            return -1;
        }
    }
    
    free(host_response);
    free(data);
    close(sock);
    return 0;
}

void VitaMTP_Stop_Broadcast(void) {
    VitaMTP_Log(VitaMTP_DEBUG, "stopping broadcast\n");
    g_stopbroadcast = 1;
}

static inline void VitaMTP_Parse_Device_Headers(char *data, wireless_vita_info_t *info, char **p_host, char **p_pin) {
    char *info_str = strtok(data, "\r\n");
    while (info_str != NULL) {
        if (strncmp(info_str, "host-id:", strlen("host-id:")) == 0) {
            if (p_host) *p_host = info_str + strlen("host-id:");
        } else if (strncmp(info_str, "device-id:", strlen("device-id:")) == 0) {
            info->deviceid = info_str + strlen("device-id:");
        } else if (strncmp(info_str, "device-type:", strlen("device-type:")) == 0) {
            info->type = info_str + strlen("device-type:");
        } else if (strncmp(info_str, "device-mac-address:", strlen("device-mac-address:")) == 0) {
            info->mac_addr = info_str + strlen("device-mac-address:");
        } else if (strncmp(info_str, "device-name:", strlen("device-name:")) == 0) {
            info->name = info_str + strlen("device-name:");
        } else if (strncmp(info_str, "pin-code:", strlen("pin-code:")) == 0) {
            if (p_pin) *p_pin = info_str + strlen("pin-code:");
        } else {
            VitaMTP_Log(VitaMTP_INFO, "Unknown field in Vita registration request: %s\n", info_str);
        }
        info_str = strtok(NULL, "\r\n");
    }
}

static int VitaMTP_Get_Wireless_Device(wireless_host_info_t *info, vita_device_t *device, unsigned int host_addr, int timeout, device_registered_callback_t is_registered, register_device_callback_t create_register_pin) {
    int s_sock;
    unsigned int slen;
    struct sockaddr_in si_host;
    struct sockaddr_in si_client;
    int flags;
    if ((s_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot create server socket\n");
        return -1;
    }
    if ((flags = fcntl(s_sock, F_GETFL, 0)) < 0) {
        flags = 0;
    }
    if (fcntl(s_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot mark server socket as non-blocking\n");
        close(s_sock);
        return -1;
    }
    memset(&si_host, 0, sizeof(si_host));
    si_host.sin_family = AF_INET;
    si_host.sin_port = htons(info->port);
    si_host.sin_addr.s_addr = host_addr ? htonl(host_addr) : htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&si_host, sizeof(si_host)) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot bind server socket\n");
        close(s_sock);
        return -1;
    }
    if (listen(s_sock, SOMAXCONN) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "cannot listen on server socket\n");
        close(s_sock);
        return -1;
    }
    
    fd_set fd;
    struct timeval time = {0};
    int ret;
    int c_sock = -1;
    char *data = NULL;
    size_t len;
    char method[20];
    int read;
    int pin = -1;
    int listen = 1;
    memset(device, 0, sizeof(vita_device_t));
    VitaMTP_Log(VitaMTP_DEBUG, "waiting for connection\n");
    while (listen) {
        FD_ZERO(&fd);
        FD_SET(s_sock, &fd);
        if (timeout) time.tv_sec = timeout;
        // use select for the timeout feature, ignore fd
        // s_sock+1 allows us to check fd "s_sock" but ignore the rest
        if ((ret = select(s_sock+1, &fd, NULL, NULL, timeout ? &time : NULL)) < 0) {
            VitaMTP_Log(VitaMTP_ERROR, "Error polling listener\n");
            break;
        } else if (ret == 0) {
            VitaMTP_Log(VitaMTP_INFO, "Listening timed out.\n");
            break;
        }
        slen = sizeof(si_client);
        if ((c_sock = accept(s_sock, (struct sockaddr *)&si_client, &slen)) < 0) {
            VitaMTP_Log(VitaMTP_ERROR, "Error accepting connection\n");
            break;
        }
        VitaMTP_Log(VitaMTP_DEBUG, "Found new client.\n");
        while (1) {
            char resp[RESPONSE_MAX_SIZE];
            if (VitaMTP_Sock_Read_All(c_sock, (unsigned char **)&data, &len, NULL, NULL) < 0) {
                VitaMTP_Log(VitaMTP_ERROR, "Error reading from client\n");
                listen = 0;
                break;
            }
            if (len == 0) {
                pin = -1; // reset any current registration
                break; // connection closed
            }
            if (sscanf(data, "%20s * HTTP/1.1\r\n%n", method, &read) < 1) {
                VitaMTP_Log(VitaMTP_ERROR, "Device request malformed: %.*s\n", (int)len, data);
                listen = 0;
                break;
            }
            if (strcmp(method, "CONNECT") == 0) {
                if (sscanf(data+read, "device-id:%32s\r\ndevice-port:%d\r\n", device->guid, &device->network_device.data_port) < 2) {
                    VitaMTP_Log(VitaMTP_ERROR, "Error parsing device request\n");
                    listen = 0;
                    break;
                }
                if (device->network_device.registered || is_registered(device->guid)) {
                    strcpy(resp, "HTTP/1.1 210 OK\r\n");
                } else {
                    strcpy(resp, "HTTP/1.1 605 NG\r\n");
                }
            } else if (strcmp(method, "SHOWPIN") == 0) {
                wireless_vita_info_t info;
                int err;
                VitaMTP_Parse_Device_Headers(data+read, &info, NULL, NULL);
                strncpy(device->guid, info.deviceid, 32);
                device->guid[32] = '\0';
                // TODO: Check if host GUID is actually our GUID
                const char *okay = "HTTP/1.1 200 OK\r\n";
                if (VitaMTP_Sock_Write_All(c_sock, (const unsigned char *)okay, strlen(okay)+1, NULL, 0) < 0) {
                    VitaMTP_Log(VitaMTP_ERROR, "Error sending request result\n");
                    listen = 0;
                    break;
                }
                if ((pin = create_register_pin(&info, &err)) < 0) {
                    sprintf(resp, "REGISTERCANCEL * HTTP/1.1\r\nerrorcode:%d\r\n", err);
                } else {
                    free(data);
                    continue;
                }
            } else if (strcmp(method, "REGISTER") == 0) {
                wireless_vita_info_t info;
                char *pin_try;
                VitaMTP_Parse_Device_Headers(data+read, &info, NULL, &pin_try);
                if (strcmp(device->guid, info.deviceid)) {
                    VitaMTP_Log(VitaMTP_ERROR, "PIN generated for device %s, but response came from %s!\n", device->guid, info.deviceid);
                    strcpy(resp, "HTTP/1.1 610 NG\r\n");
                } else if (pin < 0) {
                    VitaMTP_Log(VitaMTP_ERROR, "No PIN generated. Cannot register device %s.\n", info.deviceid);
                    strcpy(resp, "HTTP/1.1 610 NG\r\n");
                } else if (pin != atoi(pin_try)) {
                    VitaMTP_Log(VitaMTP_ERROR, "PIN mismatch. Correct: %08d, Got: %s\n", pin, pin_try);
                    strcpy(resp, "HTTP/1.1 610 NG\r\n");
                } else {
                    device->network_device.registered = 1;
                    strcpy(resp, "HTTP/1.1 200 OK\r\n");
                }
            } else if (strcmp(method, "STANDBY") == 0) {
                device->network_device.init_sockfd = c_sock;
                device->network_device.addr = si_client;
                free(data);
                close(s_sock);
                return VitaMTP_Data_Connect(device);
            } else {
                // no response needed
                if (!(strcmp(method, "REGISTERRESULT") || strcmp(method, "REGISTERCANCEL"))) {
                    VitaMTP_Log(VitaMTP_INFO, "Unkown method %s\n", method);
                }
                free(data);
                continue;
            }
            
            if (VitaMTP_Sock_Write_All(c_sock, (const unsigned char *)resp, strlen(resp)+1, NULL, 0) < 0) {
                VitaMTP_Log(VitaMTP_ERROR, "Error sending request result\n");
                listen = 0;
                break;
            }
            free(data);
        }
        close(c_sock);
    }
    free(data);
    close(c_sock);
    close(s_sock);
    return -1;
}

void VitaMTP_Release_Wireless_Device(vita_device_t *device) {
    close(device->network_device.init_sockfd);
    close(device->params->cmdfd);
    close(device->params->evtfd);
    ptp_free_params(device->params);
    free(device->params);
    free(device);
}

vita_device_t *VitaMTP_Get_First_Wireless_Vita(wireless_host_info_t *info, unsigned int host_addr, int timeout, device_registered_callback_t is_registered, register_device_callback_t create_register_pin) {
    vita_device_t *device = malloc(sizeof(vita_device_t));
    if (device == NULL) {
        VitaMTP_Log(VitaMTP_ERROR, "out of memory\n");
        return NULL;
    }
    if (VitaMTP_Get_Wireless_Device(info, device, host_addr, timeout, is_registered, create_register_pin) < 0) {
        VitaMTP_Log(VitaMTP_ERROR, "error locating Vita\n");
        return NULL;
    }
    return device;
}
