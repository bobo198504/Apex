#ifndef APEX_COMMON_SCREEN_COVER_H
#define APEX_COMMON_SCREEN_COVER_H

// A window carrying this property is an opaque screen-cover surface. The host still treats it as a fullscreen
// overlay for z-order purposes, but does not hide and later recreate lower overlay windows behind it.
#define APEX_SCREEN_COVER_PROPERTY "ApexScreenCover"

#endif
