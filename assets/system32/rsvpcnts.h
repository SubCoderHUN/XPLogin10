/*++

Copyright (c) 1996 Microsoft Corporation

Module Name:

    rsvpcnts.h

Abstract:

    Offset definition file for extensible counter objects and counters

    These "relative" offsets must start at 0 and be multiples of 2 (i.e.
    even numbers). In the Open Procedure, they will be added to the 
    "First Counter" and "First Help" values for the device they belong to, 
    in order to determine the absolute location of the counter and 
    object names and corresponding Explain text in the registry.

    This file is used by the extensible counter DLL code as well as the 
    counter name and Explain text definition file (.INI) file that is used
    by LODCTR to load the names into the registry.

Author:

    Ramesh Pabbati (rameshpa)    January 29, 1997

Revision History:

--*/

#define RSVPOBJ             0

#define RSVP_INTERFACES     2
#define RSVP_NET_SOCKETS    4
#define RSVP_TIMERS         6

#define API_SESSIONS        8
#define API_CLIENTS         10
#define API_PATH            12
#define API_RESV            14
#define API_FAILED_REQ      16
#define API_FAILED_SEND     18
#define API_QOS_NOTIFIES    20
#define API_QOS_NOTIF_BYTES 22

#define RSVPIFOBJ           24
#define TOTAL_BYTES_IN      26
#define TOTAL_BYTES_OUT     28

#define PATH_MSG_IN         30
#define RESV_MSG_IN         32 
#define PATH_ERR_MSG_IN     34 
#define RESV_ERR_MSG_IN     36 
#define PATH_TEAR_MSG_IN    38 
#define RESV_TEAR_MSG_IN    40 
#define CONFIRM_MSG_IN      42 

#define PATH_MSG_OUT        44
#define RESV_MSG_OUT        46 
#define PATH_ERR_MSG_OUT    48 
#define RESV_ERR_MSG_OUT    50 
#define PATH_TEAR_MSG_OUT   52 
#define RESV_TEAR_MSG_OUT   54 
#define CONFIRM_MSG_OUT     56 

#define ADMISS_FAILS        58
#define POLICY_FAILS        60 
#define OTHER_FAILS         62 
#define BLOCKADE_EV         64 
#define RESV_TIMEOUT        66 
#define PATH_TIMEOUT        68 

#define SND_FAILS_BIG_MSG   70
#define RCV_FAILS_BIG_MSG   72
#define SND_FAILS_NO_MEM    74
#define RCV_FAILS_NO_MEM    76

#define DROPPED_IN_MSGS     78
#define DROPPED_OUT_MSGS    80

#define ACTIVE_FLOWS        82 

#define ALLOCATED_BW        84

#define MAX_ALLOCATED_BW    86

// Everything after this point should be deleted.
// 
// #define MSIDLPMOBJ          88
// 
// #define SENDERS_ACCEPTED    90
// #define SENDER_CHG_ACCEPTED 92
// #define REJ_SND_FLOW_RATE   94
// #define REJ_SND_PEAK_RATE   96
// #define REJ_SND_TFR         98
// #define REJ_SND_TPR         100
// #define REJ_SND_ID_CHG      102
// #define REJ_SND_DURATION    104
// #define REJ_SND_FLOW_COUNT  106
// #define REJ_SND_OTHERS      108
// 
// #define RECEIVERS_ACCEPTED  110
// #define RECV_CHG_ACCEPTED   112
// #define REJ_RECV_FLOW_RATE  114
// #define REJ_RECV_PEAK_RATE  116
// #define REJ_RECV_TFR        118
// #define REJ_RECV_TPR        120
// #define REJ_RECV_ID_CHG     122
// #define REJ_RECV_DURATION   124
// #define REJ_RECV_FLOW_COUNT 126
// #define REJ_RECV_OTHERS     128
// 
// #define BAD_IDENTITY_PES    130
// 
// #define DS_CACHE_SIZE       132
// 

