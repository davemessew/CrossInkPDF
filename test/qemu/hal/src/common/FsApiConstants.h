#pragma once

#include <cstdint>

#ifdef O_RDONLY
#undef O_RDONLY
#endif
#ifdef O_WRONLY
#undef O_WRONLY
#endif
#ifdef O_RDWR
#undef O_RDWR
#endif
#ifdef O_AT_END
#undef O_AT_END
#endif
#ifdef O_APPEND
#undef O_APPEND
#endif
#ifdef O_CREAT
#undef O_CREAT
#endif
#ifdef O_TRUNC
#undef O_TRUNC
#endif
#ifdef O_EXCL
#undef O_EXCL
#endif
#ifdef O_SYNC
#undef O_SYNC
#endif

using oflag_t = uint8_t;

static constexpr oflag_t O_RDONLY = 0X00;
static constexpr oflag_t O_WRONLY = 0X01;
static constexpr oflag_t O_RDWR = 0X02;
static constexpr oflag_t O_AT_END = 0X04;
static constexpr oflag_t O_APPEND = 0X08;
static constexpr oflag_t O_CREAT = 0X10;
static constexpr oflag_t O_TRUNC = 0X20;
static constexpr oflag_t O_EXCL = 0X40;
static constexpr oflag_t O_SYNC = 0X80;
