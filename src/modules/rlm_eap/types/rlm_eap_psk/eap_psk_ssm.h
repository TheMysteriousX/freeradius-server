/* $Id$ */

/*
 * eap_psk_ssm.h
 *
 * Implementation of the Server State Machine (SSM)
 *
 * 
 * Copyright (C) France T�l�com R&D (DR&D/MAPS/NSS)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2006 The FreeRADIUS server project
 *
 */

#ifndef _EAP_PSK_SSM_H
#define _EAP_PSK_SSM_H

#include <freeradius-devel/ident.h>
RCSIDH(eap_psk_ssm_h, "$Id$")

#include "eap_psk.h"
#include "eap.h"

#if defined(__cplusplus)
extern "C"
{
#endif


// server states
typedef enum {
  INIT,       // the server state machine starts in the INIT state
  RANDSENT,
  PCHANNEL	  	
}PSK_STATE;


// information which must be kept during the EAP-PSK session
typedef struct psk_session_t {
  PSK_STATE state;                                       // state of the server state machine
  unsigned char rand_s[PSK_RANDOM_NUMBER_SIZE];          // random number generated by the server
  unsigned char *id_p;                                   // peer identity
  unsigned char ak[PSK_AK_SIZE];                         // authentication key
  unsigned char kdk[PSK_KDK_SIZE];                       // derivation key
  unsigned char tek[PSK_TEK_SIZE];                       // TEK key
  unsigned char msk[PSK_MSK_SIZE];                       // MSK key
  unsigned char emsk[PSK_EMSK_SIZE];                     // EMSK key
  unsigned int nbRetry;                                  // the current number of request re emissions
  unsigned long int pChannelReplayCounter;               // the p-channel replay counter
  unsigned char extType;                                // the extension type if evolved authentication is used, else 0
  unsigned char authStatus;                             // the latest R flag sent by the server
  unsigned char isSupportedExt;                          // 0 if the peer doesn't support the specified extension
}PSK_SESSION;


/** 
 *@memo		this function is the entry point of the server state machine
 *@param        conf, pointer to the current configuration of EAP-PSK
 *@param        session, pointer to a structure which contains information session
 *@param        recvPacket, pointer to a received EAP_PACKET
 *@param        sentPacket, pointer to the EAP_PACKET to send
 *@return       0 if an error has occured
 */
  int pskProcess(PSK_CONF *conf, PSK_SESSION *session, EAP_PACKET *recvPacket, EAP_PACKET *sentPacket);


/** 
 *@memo		this function corresponds to the first state of the server state machine
 *@param        conf, pointer to the current configuration of EAP-PSK
 *@param        session, pointer to a structure which contains information session
 *@param        sentPacket, pointer to the EAP_PACKET to send
 *@return       0 if an error has occured
 */
  int pskInit(PSK_CONF *conf, PSK_SESSION *session, EAP_PACKET *sentPacket);


/** 
 *@memo		this function corresponds to the second state of the server state machine
 *@param        conf, pointer to the current configuration of EAP-PSK
 *@param        session, pointer to a structure which contains information session
 *@param        recvPacket, pointer to a received EAP_PACKET
 *@param        sentPacket, pointer to the EAP_PACKET to send
 *@return       0 if an error has occured
 */
  int pskRandSent(PSK_CONF *conf, PSK_SESSION *session, EAP_PACKET *recvPacket, EAP_PACKET *sentPacket);


/** 
 *@memo		this function corresponds to the third state of the server state machine
 *@param        conf, pointer to the current configuration of EAP-PSK
 *@param        session, pointer to a structure which contains information session
 *@param        recvPacket, pointer to a received EAP_PACKET
 *@param        sentPacket, pointer to the EAP_PACKET to send
 *@return       0 if no error has occured
 */
  int pskPChannel(PSK_CONF *conf, PSK_SESSION *session, EAP_PACKET *recvPacket, EAP_PACKET *sentPacket);			


/** 
 *@memo		this function contains the extension to EAP-PSK
 *@param        conf, pointer to the current configuration of EAP-PSK
 *@param        session, pointer to a structure which contains information session
 *@param        receivedStatus, the latest R flag sent by the peer
 *@param        dataIn, pointer to the received data
 *@param        sizeDataIn, size of the received data
 *@param        dataOut, pointer to a pointer that points data to send
 *@param        sizeDataOut, pointer to the size of data to be sent (sizeDataOut must be at most equal to EXT_PAYLAOD_MAX_LEN)
 *@return       0 if an error has occured
 */
  int pskExtension(PSK_CONF *conf, PSK_SESSION *session, unsigned short receivedStatus, void *dataIn, int sizeDataIn, void **dataOut, int *sizeDataOut);


/** 
 *@memo		this function frees an existing session from memory
 *@param        opaque, pointer to a structure which contains information session
 */
  void pskFreeSession(void *opaque);
  


#if defined(__cplusplus)
}
#endif

#endif /*_EAP_PSK_SSM_H*/
