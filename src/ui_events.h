#pragma once

#define ACE_EVENT_DELETE_LINE   1
#define ACE_EVENT_ATTACH_TAPE   2   /* data1 = malloc'd filepath */
#define ACE_EVENT_REWIND_TAPE   9
#define ACE_EVENT_INVERSE_VIDEO 3
#define ACE_EVENT_GRAPHICS      4
#define ACE_EVENT_SPOOL         5   /* data1 = malloc'd filepath */
#define ACE_EVENT_RESET         6
#define ACE_EVENT_BREAK         7
#define ACE_EVENT_PASTE         8
#define ACE_EVENT_COPY          10
