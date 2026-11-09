#ifndef APEX_APPPROXY_WINDIVERT_API_H
#define APEX_APPPROXY_WINDIVERT_API_H

// Minimal ABI declarations for the official WinDivert 2.2 user-mode API. The driver and WinDivert.dll are NOT
// bundled with Apex: users download WinDivert themselves and point AppProxy at that folder. See
// https://github.com/basil00/WinDivert (LGPLv3 / GPLv2 dual licence).
#include <windows.h>
#include <cstdint>

#define WINDIVERT_FLAG_SNIFF 0x0001
#define WINDIVERT_FLAG_DROP 0x0002
#define WINDIVERT_FLAG_RECV_ONLY 0x0004
#define WINDIVERT_FLAG_NO_INSTALL 0x0010

typedef struct
{
  INT64 Timestamp;
  UINT32 Layer : 8;
  UINT32 Event : 8;
  UINT32 Sniffed : 1;
  UINT32 Outbound : 1;
  UINT32 Loopback : 1;
  UINT32 Impostor : 1;
  UINT32 IPv6 : 1;
  UINT32 IPChecksum : 1;
  UINT32 TCPChecksum : 1;
  UINT32 UDPChecksum : 1;
  UINT32 Reserved1 : 8;
  UINT32 Reserved2;
  union
  {
    struct
    {
      UINT32 IfIdx;
      UINT32 SubIfIdx;
    } Network;
    UINT8 Reserved3[64];
  };
} WINDIVERT_ADDRESS, *PWINDIVERT_ADDRESS;

typedef HANDLE(WINAPI *WinDivertOpenFn)(const char *, int, INT16, UINT64);
typedef BOOL(WINAPI *WinDivertRecvFn)(HANDLE, VOID *, UINT, UINT *, WINDIVERT_ADDRESS *);
typedef BOOL(WINAPI *WinDivertSendFn)(HANDLE, const VOID *, UINT, UINT *, const WINDIVERT_ADDRESS *);
typedef BOOL(WINAPI *WinDivertShutdownFn)(HANDLE, int);
typedef BOOL(WINAPI *WinDivertCloseFn)(HANDLE);
typedef BOOL(WINAPI *WinDivertSetParamFn)(HANDLE, int, UINT64);
typedef BOOL(WINAPI *WinDivertHelperCalcChecksumsFn)(VOID *, UINT, WINDIVERT_ADDRESS *, UINT64);

struct WinDivertApi
{
  HMODULE dll;
  WinDivertOpenFn Open;
  WinDivertRecvFn Recv;
  WinDivertSendFn Send;
  WinDivertShutdownFn Shutdown;
  WinDivertCloseFn Close;
  WinDivertSetParamFn SetParam;
  WinDivertHelperCalcChecksumsFn CalcChecksums;
};

typedef struct
{
  UINT8 HdrLength : 4;
  UINT8 Version : 4;
  UINT8 TOS;
  UINT16 Length;
  UINT16 Id;
  UINT16 FragOff0;
  UINT8 TTL;
  UINT8 Protocol;
  UINT16 Checksum;
  UINT32 SrcAddr;
  UINT32 DstAddr;
} WINDIVERT_IPHDR;

typedef struct
{
  UINT8 TrafficClass0 : 4;
  UINT8 Version : 4;
  UINT8 FlowLabel0 : 4;
  UINT8 TrafficClass1 : 4;
  UINT16 FlowLabel1;
  UINT16 Length;
  UINT8 NextHdr;
  UINT8 HopLimit;
  UINT32 SrcAddr[4];
  UINT32 DstAddr[4];
} WINDIVERT_IPV6HDR;

typedef struct
{
  UINT16 SrcPort;
  UINT16 DstPort;
  UINT32 SeqNum;
  UINT32 AckNum;
  UINT16 Reserved1 : 4;
  UINT16 HdrLength : 4;
  UINT16 Fin : 1;
  UINT16 Syn : 1;
  UINT16 Rst : 1;
  UINT16 Psh : 1;
  UINT16 Ack : 1;
  UINT16 Urg : 1;
  UINT16 Reserved2 : 6;
  UINT16 Window;
  UINT16 Checksum;
  UINT16 UrgPtr;
} WINDIVERT_TCPHDR;

typedef struct
{
  UINT16 SrcPort;
  UINT16 DstPort;
  UINT16 Length;
  UINT16 Checksum;
} WINDIVERT_UDPHDR;

#endif
