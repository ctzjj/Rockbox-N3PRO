/* SWD v2.1 (the driver) 系数装载器 0x1000832c —— 与驱动逐指令等价
 *
 * 驱动里它由 setter 0x10008e00 (state[0x10]=sel) 与 0x10008e0c (state[0x14])
 * 尾部 `b 0x1000832c` 调用；开机时耳机/扬声器检测函数会触发。 
 *
 * 作用：按 (state[0x14],state[8]) 与 state[0x10] 从 blob 静态子表里取
 *       三组系数 + 一组 Q，拷进 dest 池 (基址 state[0x138])，
 *       并把 DSP 用的实例指针 state[0x34]/[0xc0]/[0xc4]/[0x118] 指向 dest 池。
 */
#include "swd21.h"

static inline int32_t LD_(uint32_t a){
    int32_t v;
    __builtin_memcpy(&v, swd_map(a), 4);
    return v;
}
static inline void ST_(uint32_t a, int32_t v){
    __builtin_memcpy(swd_map(a), &v, 4);
}

void swd21_coef_load(uint32_t S){
    uint32_t v14 = (uint32_t)LD_(S + 0x14);
    uint32_t v8  = (uint32_t)LD_(S + 0x08);
    uint32_t P2p = 0, P3p = 0;
    int have = 0;

    /* 0x832c..0x838c: 按 (state[0x14],state[8]) 选 (P2,P3) 对 */
    if (v14 == 1u){
        if (v8 == 0u)      { P3p = (uint32_t)LD_(S + 0x120); P2p = (uint32_t)LD_(S + 0xa4); have = 1; }
        else if (v8 == 1u) { P3p = (uint32_t)LD_(S + 0x11c); P2p = (uint32_t)LD_(S + 0x9c); have = 1; }
    } else {
        if (v8 == 0u)      { P3p = (uint32_t)LD_(S + 0x128); P2p = (uint32_t)LD_(S + 0xb4); have = 1; }
        else if (v8 == 1u) { P3p = (uint32_t)LD_(S + 0x124); P2p = (uint32_t)LD_(S + 0xac); have = 1; }
    }
    if (have){
        ST_(S + 0xcc,  (int32_t)P2p);
        ST_(S + 0x118, (int32_t)P3p);
    }

    /* 0x8390..0x8444: 按 state[0x10] (sel) 选 (P1,P2,P3) */
    uint32_t sel = (uint32_t)LD_(S + 0x10);
    uint32_t r3, r2, r1;
    switch (sel){
      case 1: r3=(uint32_t)LD_(S+0x44); r2=(uint32_t)LD_(S+0x48); r1=(uint32_t)LD_(S+0x4c); break;
      case 2: r3=(uint32_t)LD_(S+0x50); r2=(uint32_t)LD_(S+0x54); r1=(uint32_t)LD_(S+0x58); break;
      case 3: r3=(uint32_t)LD_(S+0x5c); r2=(uint32_t)LD_(S+0x60); r1=(uint32_t)LD_(S+0x64); break;
      case 4: r3=(uint32_t)LD_(S+0x68); r2=(uint32_t)LD_(S+0x6c); r1=(uint32_t)LD_(S+0x70); break;
      case 5: r3=(uint32_t)LD_(S+0x74); r2=(uint32_t)LD_(S+0x78); r1=(uint32_t)LD_(S+0x7c); break;
      case 6: r3=(uint32_t)LD_(S+0x80); r2=(uint32_t)LD_(S+0x84); r1=(uint32_t)LD_(S+0x88); break;
      case 7: r3=(uint32_t)LD_(S+0x8c); r2=(uint32_t)LD_(S+0x90); r1=(uint32_t)LD_(S+0x94); break;
      default:r3=(uint32_t)LD_(S+0x38); r2=(uint32_t)LD_(S+0x3c); r1=(uint32_t)LD_(S+0x40); break;
    }
    ST_(S + 0xc4, (int32_t)r1);
    uint32_t cur = (uint32_t)LD_(S + 0x138);     /* dest 游标基址 */
    ST_(S + 0xc0, (int32_t)r2);
    ST_(S + 0x34, (int32_t)r3);                  /* P1 */

    /* P1: 2 words -> cur+0x1c */
    uint32_t p = (uint32_t)LD_(S + 0x34);
    ST_(cur + 0x1c, LD_(p));
    ST_(S + 0x34, (int32_t)(p + 4));
    ST_(cur + 0x20, LD_(p + 4));
    ST_(S + 0x34, (int32_t)(cur + 0x1c));        /* 0x8474 */

    /* P2: 3 words -> cur+0x2c */
    p = (uint32_t)LD_(S + 0xc0);
    ST_(cur + 0x2c, LD_(p));
    ST_(S + 0xc0, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0xc0);
    ST_(cur + 0x30, LD_(p));
    ST_(S + 0xc0, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0xc0);
    ST_(cur + 0x34, LD_(p));
    ST_(S + 0xc0, (int32_t)(cur + 0x2c));        /* 0x84ac */

    /* P3: 3 words -> cur+0x40 */
    p = (uint32_t)LD_(S + 0xc4);
    ST_(cur + 0x40, LD_(p));
    ST_(S + 0xc4, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0xc4);
    ST_(cur + 0x44, LD_(p));
    ST_(S + 0xc4, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0xc4);
    ST_(cur + 0x48, LD_(p));
    ST_(S + 0xc4, (int32_t)(cur + 0x40));        /* 0x84e4 */

    /* Q (state[0x118]): 5 words -> cur+0x54 */
    p = (uint32_t)LD_(S + 0x118);
    ST_(cur + 0x54, LD_(p));
    ST_(S + 0x118, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0x118);
    ST_(cur + 0x58, LD_(p));
    ST_(S + 0x118, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0x118);
    ST_(cur + 0x5c, LD_(p));
    ST_(S + 0x118, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0x118);
    ST_(cur + 0x60, LD_(p));
    ST_(S + 0x118, (int32_t)(p + 4));
    p = (uint32_t)LD_(S + 0x118);
    ST_(cur + 0x64, LD_(p));
    ST_(S + 0x118, (int32_t)(cur + 0x54));       /* 0x8540 */
}
