/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef ZIG_BUFFER_HPP
#define ZIG_BUFFER_HPP

#include "list.hpp"

#include <assert.h>
#include <stdint.h>

struct Buf {
    ZigList<char> list;
};

Buf *buf_sprintf(const char *format, ...)
    __attribute__ ((format (printf, 1, 2)));

static inline int buf_len(Buf *buf) {
    return buf->list.length - 1;
}

static inline char *buf_ptr(Buf *buf) {
    return buf->list.items;
}

static inline void buf_resize(Buf *buf, int new_len) {
    buf->list.resize(new_len + 1);
    buf->list.at(buf_len(buf)) = 0;
}

static inline Buf *buf_alloc(void) {
    Buf *buf = allocate<Buf>(1);
    buf_resize(buf, 0);
    return buf;
}

static inline Buf *buf_alloc_fixed(int size) {
    Buf *buf = allocate<Buf>(1);
    buf_resize(buf, size);
    return buf;
}

static inline void buf_deinit(Buf *buf) {
    buf->list.deinit();
}

static inline Buf *buf_from_mem(char *ptr, int len) {
    Buf *buf = allocate<Buf>(1);
    buf->list.resize(len + 1);
    memcpy(buf_ptr(buf), ptr, len);
    buf->list.at(buf_len(buf)) = 0;
    return buf;
}

static inline Buf *buf_from_str(char *str) {
    return buf_from_mem(str, strlen(str));
}

static inline Buf *buf_slice(Buf *in_buf, int start, int end) {
    assert(start >= 0);
    assert(end >= 0);
    assert(start < buf_len(in_buf));
    assert(end <= buf_len(in_buf));
    Buf *out_buf = allocate<Buf>(1);
    out_buf->list.resize(end - start + 1);
    memcpy(buf_ptr(out_buf), buf_ptr(in_buf) + start, end - start);
    out_buf->list.at(buf_len(out_buf)) = 0;
    return out_buf;
}

static inline void buf_append_str(Buf *buf, const char *str, int str_len) {
    if (str_len == -1)
        str_len = strlen(str);

    int old_len = buf_len(buf);
    buf_resize(buf, old_len + str_len);
    memcpy(buf_ptr(buf) + old_len, str, str_len);
    buf->list.at(buf_len(buf)) = 0;
}

static inline void buf_append_buf(Buf *buf, Buf *append_buf) {
    buf_append_str(buf, buf_ptr(append_buf), buf_len(append_buf));
}

// TODO this method needs work
static inline Buf *buf_dirname(Buf *buf) {
    if (buf_len(buf) <= 2)
        zig_panic("TODO buf_dirname small");
    int last_index = buf_len(buf) - 1;
    if (buf_ptr(buf)[buf_len(buf) - 1] == '/') {
        last_index = buf_len(buf) - 2;
    }
    for (int i = last_index; i >= 0; i -= 1) {
        uint8_t c = buf_ptr(buf)[i];
        if (c == '/') {
            return buf_slice(buf, 0, i);
        }
    }
    zig_panic("TODO buf_dirname no slash");
}


#endif