#pragma once
#include "3ds.h"

// libctru now ships its own AM_TicketInfo and AMAPP_* with other signatures; keep these private.
#define AM_TicketInfo ABS_AM_TicketInfo
#define AM_GetTitleInfoIgnorePlatform ABS_AM_GetTitleInfoIgnorePlatform
#define AMAPP_FindDLCContentInfos ABS_AMAPP_FindDLCContentInfos
#define AMAPP_GetDLCTitleInfos ABS_AMAPP_GetDLCTitleInfos
#define AMAPP_ListDataTitleTicketInfos ABS_AMAPP_ListDataTitleTicketInfos
#define AMAPP_GetPatchTitleInfos ABS_AMAPP_GetPatchTitleInfos

typedef struct
{
    u64 title_id;
    u64 ticket_id;
    u16 version;
    u16 unused;
    u32 size;
} AM_TicketInfo;

Result AM_GetTitleInfoIgnorePlatform(FS_MediaType mediatype, u32 titleCount, u64 *titleIds, AM_TitleEntry *titleInfo);

Result AMAPP_FindDLCContentInfos(FS_MediaType mediatype, u64 title_id, u32 contentCount, u16 *contentIds, AM_ContentInfo *contentInfos);

Result AMAPP_GetDLCTitleInfos(FS_MediaType mediatype, u32 titleCount, u64 *titleIds, AM_TitleEntry *titleInfo);

Result AMAPP_ListDataTitleTicketInfos(u32* ticketReadCount, u64 titleID, u32 ticketInfoCount, u32 offset, AM_TicketInfo* ticketInfos);

Result AMAPP_GetPatchTitleInfos(FS_MediaType mediatype, u32 titleCount, u64 *titleIds, AM_TitleEntry *titleInfo);