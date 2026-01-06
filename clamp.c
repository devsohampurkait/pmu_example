#ifndef CLAMP_H
#define CLAMP_H

#define clamp(val, lo, hi)                     \
({                                             \
    __typeof__(val) _v = (val);                \
    __typeof__(lo)  _lo = (lo);                \
    __typeof__(hi)  _hi = (hi);                \
    _v < _lo ? _lo : (_v > _hi ? _hi : _v);    \
})

#endif
