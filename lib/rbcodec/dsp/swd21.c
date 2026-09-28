/* SWD v2.1 (the driver) DSP core —— 0x1000b000 定点 C 实现
 *
 * 与已验证逐位一致的 Python (swd_dsp.py) 等价。
 * 内存访问经 swd_map()（用户提供），把 32 位地址映射为主机指针；
 * 在 MCU 上可直接换成身份映射/真实指针。
 */
#include "swd21.h"

/* 用户提供：把 32 位地址映射为主机指针 */
extern uint8_t *swd_map(uint32_t addr);

#define M32 0xFFFFFFFFu

static inline int32_t s32v(int64_t x){ return (int32_t)(uint32_t)((uint64_t)x & M32); }

static inline void smull(int32_t a, int32_t b, int32_t *lo, int32_t *hi){
    int64_t p = (int64_t)a * (int64_t)b;
    *lo = (int32_t)(uint32_t)((uint64_t)p & M32);
    *hi = (int32_t)(uint32_t)((uint64_t)p >> 32);
}

static inline void smlal(int32_t *lo, int32_t *hi, int32_t a, int32_t b){
    int64_t acc = ((int64_t)(int32_t)*hi << 32) | (uint32_t)*lo;
    acc += (int64_t)a * (int64_t)b;
    *lo = (int32_t)(uint32_t)((uint64_t)acc & M32);
    *hi = (int32_t)(uint32_t)((uint64_t)acc >> 32);
}

static inline int32_t lsl(int32_t v, int32_t n){
    n &= 0xFF;
    if (n == 0) return v;
    if (n >= 32) return 0;
    return (int32_t)((uint32_t)v << n);
}

static inline int32_t asr(int32_t v, int32_t n){
    n &= 0xFF;
    if (n == 0) return v;
    if (n >= 32) return v < 0 ? -1 : 0;
    return v >> n;
}

static inline int32_t sat_out(int32_t v, int32_t r7){
    int32_t r2;
    if (v >= 0x08000000)        r2 = (int32_t)0x7FFFFFFF;
    else if (v <= (int32_t)-0x08000000) r2 = (int32_t)0x80000000;
    else                        r2 = (int32_t)((uint32_t)v << 4);
    return asr(r2, r7);
}

static inline int32_t clamp_pos(int32_t r7, int32_t ip){
    return (ip > 0x07FFFFFF) ? (int32_t)0x7FFFFFFF : r7;
}

static inline int32_t abs32(int32_t v){
    return (v < 0) ? (int32_t)(uint32_t)(-(uint32_t)v) : v;
}

static inline int32_t LD(uint32_t a){
    int32_t v;
    __builtin_memcpy(&v, swd_map(a), 4);
    return v;
}
static inline void ST(uint32_t a, int32_t v){
    __builtin_memcpy(swd_map(a), &v, 4);
}
static inline uint32_t LDU16(uint32_t a){
    uint16_t v;
    __builtin_memcpy(&v, swd_map(a), 2);
    return v;
}

void swd21_process(uint32_t state, uint32_t buf){
    int32_t r0 = (int32_t)state, r1 = (int32_t)buf, r2, r3, r4, r5, r6, r7, r8;
    int32_t r9, r10, r11, ip;
    uint32_t lr = buf;
    (void)r9; (void)r11; /* transcription residue, never referenced */
    int32_t lo, hi, fp;

    r2 = LD((uint32_t)r0 + 4);
    r3 = (int32_t)LDU16((uint32_t)r0);
    fp = r3;

    /* ---- bypass ---- */
    if (r2 == 0){
        int32_t n = fp;
        r8 = LD((uint32_t)r0 + 0x110);
        while (n){
            int32_t x4 = LD(lr), x5 = LD(lr + 4);
            smull(r8, x4, &lo, &hi); x4 = lsl(hi, 2);
            smull(r8, x5, &lo, &hi); x5 = lsl(hi, 2);
            ST(lr, x4); ST(lr + 4, x5);
            lr += 8; n = (int32_t)((uint32_t)n - 1);
        }
        return;
    }

    r0 = (int32_t)((uint32_t)r0 + 0xe0);
    /* v2.1: 无 ldr fp,[r0,#0xfc]（输入移位硬编码 12） */

    for (;;){
        /* b04c */
        r4 = LD((uint32_t)r1);
        r5 = LD((uint32_t)r1 + 4);
        r6 = LD((uint32_t)r0 + 0x1c);
        if (r2 > 0){                       /* v2.1: lslgt r4,r4,#0xc */
            r4 = lsl(r4, 12); r5 = lsl(r5, 12);
        }
        r8 = LD((uint32_t)r0 + 0x30);
        smull(r8, r4, &lo, &hi); r4 = (int32_t)(((uint32_t)lo >> 30) | ((uint32_t)hi << 2));
        smull(r8, r5, &lo, &hi); r5 = (int32_t)(((uint32_t)lo >> 30) | ((uint32_t)hi << 2));
        r8 = 0x20000000;
        if (r6 == 1){
            smull(r8, r4, &lo, &hi); r4 = lsl(hi, 1);
            smull(r8, r5, &lo, &hi); r5 = lsl(hi, 1);
        } else {
            ST((uint32_t)r0 + 0x28, 0);
        }
        ST((uint32_t)r0 + 0x10, r4); ST((uint32_t)r0 + 0x14, r5);
        ST((uint32_t)r0, r1); ST((uint32_t)r0 + 4, r3);
        r3 = r4; r4 = r5;
        /* b0b8 M 支路 1 阶 IIR {c0,c1,state} */
        {
            int32_t p2 = LD((uint32_t)r0 - 0xac);
            int32_t M = (int32_t)((uint32_t)r4 + (uint32_t)r3);
            int32_t a0 = LD((uint32_t)p2), a1 = LD((uint32_t)p2 + 4), a2 = LD((uint32_t)p2 + 8);
            int32_t slv;
            r1 = M;
            smull(a0, r1, &lo, &hi);
            slv = (int32_t)((uint32_t)a2 + (uint32_t)lsl(hi, 1));   /* v2.1: sl = c2 + (hi<<1) */
            smlal(&lo, &hi, slv, a1);
            r1 = lsl(hi, 1);
            ST((uint32_t)p2 + 8, r1);
            ST((uint32_t)r0 + 0x18, slv);                            /* v2.1: 存 sl */
            ip = (int32_t)((uint32_t)r3 - (uint32_t)r4);   /* S */
            r3 = (int32_t)((uint32_t)r3 - (uint32_t)slv);
            r4 = (int32_t)((uint32_t)r4 - (uint32_t)slv);
            r2 = p2;
        }
        {
            int32_t mode = LD((uint32_t)r0 + 0x64);
            if (mode == 0){
                /* ============ mode 0 (0xb0f0) ============ */
                int32_t sav3 = r3, sav4 = r4, ipv;
                int32_t I = LD((uint32_t)r0 + 0x38);
                int32_t c0 = LD((uint32_t)I), c1 = LD((uint32_t)I+4), c2 = LD((uint32_t)I+8);
                int32_t c3 = LD((uint32_t)I+0xc), c4 = LD((uint32_t)I+0x10);
                int32_t s1 = LD((uint32_t)I+0x14), s2 = LD((uint32_t)I+0x18);
                smull(c0, ip, &lo, &hi);
                r3 = (int32_t)((uint32_t)s1 + (uint32_t)lsl(hi, 3));
                smull(r3, c1, &lo, &hi);
                smlal(&lo, &hi, c2, ip);
                s1 = (int32_t)((uint32_t)s2 + (uint32_t)lsl(hi, 3)); ST((uint32_t)I+0x14, s1);
                smull(r3, c3, &lo, &hi);
                smlal(&lo, &hi, ip, c4);
                s2 = lsl(hi, 3); ST((uint32_t)I+0x18, s2);
                {
                    int32_t F = LD((uint32_t)r0 - 0x28);
                    int32_t d0 = LD((uint32_t)F), d1 = LD((uint32_t)F+4), d2 = LD((uint32_t)F+8);
                    smull(r3, d0, &lo, &hi);
                    r2 = (int32_t)((uint32_t)d2 + (uint32_t)lsl(hi, 2));
                    smull(r2, d1, &lo, &hi);
                    r8 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(hi, 2));
                    {
                        int32_t lr_ = LD((uint32_t)r0 - 0xc8);       /* SWDLevel */
                        int32_t sl_c, fp_flag;
                        ST((uint32_t)F + 8, r8);
                        sl_c = LD((uint32_t)r0 + 0x6c);              /* SWDCenter */
                        fp_flag = LD((uint32_t)r0 + 0x68);
                        r3 = sav3; r4 = sav4;
                        smull(r2, lr_, &lo, &hi); ipv = hi;
                        r1 = LD((uint32_t)r0 - 0xac);
                        r4 = (int32_t)((uint32_t)r3 + (uint32_t)r4);
                        if (fp_flag == 1){
                            int32_t P = LD((uint32_t)r0 - 0x24);
                            int32_t k0 = LD((uint32_t)P), k1 = LD((uint32_t)P+4), k2 = LD((uint32_t)P+8);
                            int32_t k3 = LD((uint32_t)P+0xc), k4 = LD((uint32_t)P+0x10), aa, bb;
                            smull(k0, r4, &lo, &hi); aa = hi;
                            r1 = (int32_t)((uint32_t)k3 + (uint32_t)lsl(aa, 2));
                            smull(k1, r1, &lo, &hi);
                            smlal(&lo, &hi, r4, k2); aa = hi;
                            bb = lsl(aa, 2);
                            ST((uint32_t)P + 0xc, bb); ST((uint32_t)P + 0x10, k4);
                            sl_c = LD((uint32_t)r0 + 0x6c);
                            smull(r1, LD((uint32_t)r0 - 0xd4), &lo, &hi);
                            r5 = lsl(hi, 1);
                            smull(r4, sl_c, &lo, &hi); hi = hi;
                            r1 = LD((uint32_t)r0 + 0x18);
                            r3 = (int32_t)((uint32_t)r5 + (uint32_t)lsl(hi, 1));
                            r5 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(ipv, 1));
                            r6 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(ipv, 1));
                            r3 = r5; r4 = r6;
                        } else {
                            int32_t v;
                            smull(r4, sl_c, &lo, &hi); v = hi;
                            r1 = LD((uint32_t)r0 + 0x18);
                            r3 = lsl(v, 1);
                            r5 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(ipv, 1));
                            r6 = (int32_t)((uint32_t)r3 - (uint32_t)lsl(ipv, 1));
                            r3 = r5; r4 = r6;
                        }
                    }
                }
            } else if (mode == 1){
                /* ============ mode 1 (0xb1e0) ============ */
                int32_t I = LD((uint32_t)r0 + 0x38);
                int32_t v8 = LD((uint32_t)I + 0x14);
                r8 = v8;
                smull(r3, LD(0x1000d8b4), &lo, &hi); r2 = (int32_t)((uint32_t)r8 + (uint32_t)lsl(hi, 2));
                smull(r2, LD(0x1000d8b8), &lo, &hi); r8 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(hi, 2));
                ST((uint32_t)I + 0x14, r8);
                r8 = LD((uint32_t)I + 0x18);
                smull(r2, LD(0x1000d8bc), &lo, &hi); r10 = (int32_t)((uint32_t)r8 + (uint32_t)lsl(hi, 2));
                smull(r10, LD(0x1000d8c0), &lo, &hi); r8 = (int32_t)((uint32_t)r2 + (uint32_t)lsl(hi, 2));
                ST((uint32_t)I + 0x18, r8);
                {
                    int32_t F = LD((uint32_t)r0 - 0x28);
                    int32_t f0 = LD((uint32_t)F + 8);
                    int32_t fpv, r5v;
                    smull(r4, LD(0x1000d8c4), &lo, &hi); fpv = (int32_t)((uint32_t)f0 + (uint32_t)lsl(hi, 2));
                    smull(fpv, LD(0x1000d8c8), &lo, &hi); r8 = (int32_t)((uint32_t)r4 + (uint32_t)lsl(hi, 2));
                    ST((uint32_t)F + 8, r8);
                    r2 = LD((uint32_t)r0 + 0x74);
                    smull(r2, r10, &lo, &hi); r3 = lsl(hi, 1);
                    smull(fpv, r2, &lo, &hi); r4 = lsl(hi, 1);
                    ip = LD((uint32_t)r0 - 0xc8);
                    {
                        int32_t sl2 = (int32_t)((uint32_t)r3 - (uint32_t)r4);
                        int32_t f2;
                        smull(ip, sl2, &lo, &hi); f2 = lsl(hi, 1);
                        smull(f2, 0x5051eb85, &lo, &hi); r5v = lsl(hi, 3);
                    }
                    r1 = LD((uint32_t)r0 + 0x18);
                    r3 = (int32_t)((uint32_t)r3 + (uint32_t)r5v);
                    r4 = (int32_t)((uint32_t)r4 - (uint32_t)r5v);
                }
            } else {
                /* ============ mode >=2 (0xb294) ============ */
                int32_t EB = LD((uint32_t)r0 + 0x4c);
                int32_t e0 = LD((uint32_t)EB), e1 = LD((uint32_t)EB+4), e2 = LD((uint32_t)EB+8);
                int32_t acc = 0; int first = 1;
                r2 = (int32_t)((uint32_t)r3 - (uint32_t)r4);
                r4 = (int32_t)((uint32_t)r3 + (uint32_t)r4);
                smull(r4, e0, &lo, &hi); r3 = (int32_t)((uint32_t)e2 + (uint32_t)lsl(hi, 2));
                smull(r3, e1, &lo, &hi); r8 = (int32_t)((uint32_t)lsl(hi, 2) - (uint32_t)r4);
                ST((uint32_t)EB + 8, r8);
                {
                    int idxs[3]; int k;
                    int32_t t1 = 0;
                    idxs[0] = 0x50; idxs[1] = 0x54; idxs[2] = 0x58;
                    for (k = 0; k < 3; k++){
                        int32_t base = LD((uint32_t)r0 + idxs[k]);
                        int32_t g0 = LD((uint32_t)base), g1 = LD((uint32_t)base+4), g2 = LD((uint32_t)base+8);
                        int32_t g3 = LD((uint32_t)base+0xc), g4 = LD((uint32_t)base+0x10);
                        int32_t gs1 = LD((uint32_t)base+0x14), gs2 = LD((uint32_t)base+0x18);
                        smull(g0, r2, &lo, &hi); t1 = (int32_t)((uint32_t)gs1 + (uint32_t)lsl(hi, 2));
                        smull(t1, g1, &lo, &hi);
                        smlal(&lo, &hi, g2, r2);
                        gs1 = (int32_t)((uint32_t)gs2 + (uint32_t)lsl(hi, 2)); ST((uint32_t)base+0x14, gs1);
                        smull(t1, g3, &lo, &hi);
                        smlal(&lo, &hi, r2, g4);
                        gs2 = lsl(hi, 2); ST((uint32_t)base+0x18, gs2);
                        if (first){ acc = t1; first = 0; } else { acc = (int32_t)((uint32_t)acc + (uint32_t)t1); }
                    }
                }
                r4 = acc;
                r5 = LD((uint32_t)r0 - 0xc8);
                smull(r4, r5, &lo, &hi); r2 = lsl(hi, 1);
                r3 = (int32_t)((uint32_t)r2 + (uint32_t)r3);
                r1 = LD((uint32_t)r0 + 0x18);
                r4 = r3;
            }
        }

        /* ============ common tail 0xb36c ============ */
        r2 = LD((uint32_t)r0 - 0xac);
        {
            int32_t b0 = LD((uint32_t)r2), b1 = LD((uint32_t)r2+4), b2 = LD((uint32_t)r2+8), b3 = LD((uint32_t)r2+0xc);
            (void)b2;
            int32_t slv;
            smull(b0, r1, &lo, &hi);
            slv = (int32_t)((uint32_t)b3 + (uint32_t)lsl(hi, 1));   /* v2.1: sl = b3 + (hi<<1) */
            smlal(&lo, &hi, slv, b1);
            r7 = lsl(hi, 1);
            ip = LD((uint32_t)r0 - 0x20);
            ST((uint32_t)r2 + 0xc, r7); ST((uint32_t)r0 + 0x18, slv);
            {
                int32_t p0 = LD((uint32_t)ip), p1 = LD((uint32_t)ip+4), p2 = LD((uint32_t)ip+8);
                int32_t p3 = LD((uint32_t)ip+0xc), p4 = LD((uint32_t)ip+0x10);
                int32_t fpv, r1n, r1o;
                smull(p0, b3, &lo, &hi); fpv = (int32_t)((uint32_t)p3 + (uint32_t)lsl(hi, 2));
                r1n = (int32_t)(0u - (uint32_t)hi);
                { int32_t l3 = (int32_t)0x80000000, h3 = r1n;
                  smlal(&l3, &h3, fpv, p2); r1o = lsl(h3, 2); }
                ST((uint32_t)ip + 0x10, r1o);
                { int32_t lo2 = (int32_t)0x80000000, hi2 = 0;
                  smlal(&lo2, &hi2, fpv, p1);
                  r1o = (int32_t)((uint32_t)p4 + (uint32_t)lsl(hi2, 2)); }
                ST((uint32_t)ip + 0xc, r1o);
                {
                    int32_t ip2 = LD((uint32_t)r0 - 0x1c);
                    int32_t q0 = LD((uint32_t)ip2), q1 = LD((uint32_t)ip2+4), q2 = LD((uint32_t)ip2+8);
                    int32_t q3 = LD((uint32_t)ip2+0xc), q4 = LD((uint32_t)ip2+0x10);
                    int32_t ipv, r1n2, r1o2;
                    smull(q0, b3, &lo, &hi); ipv = (int32_t)((uint32_t)q3 + (uint32_t)lsl(hi, 2));
                    r1n2 = (int32_t)(0u - (uint32_t)hi);
                    { int32_t l3 = (int32_t)0x80000000, h3 = r1n2;
                      smlal(&l3, &h3, ipv, q2); r1o2 = lsl(h3, 2); }
                    ST((uint32_t)ip2 + 0x10, r1o2);
                    { int32_t l4 = (int32_t)0x80000000, h4 = 0;
                      smlal(&l4, &h4, ipv, q1);
                      r1o2 = (int32_t)((uint32_t)q4 + (uint32_t)lsl(h4, 2)); }
                    ST((uint32_t)ip2 + 0xc, r1o2);
                    fpv = (int32_t)(0u - (uint32_t)((uint32_t)fpv + (uint32_t)ipv));
                }
                /* ---- envelope ---- */
                {
                    int32_t env = LD((uint32_t)r0 - 0xb8);
                    int32_t c_att = LD((uint32_t)r0 - 0xc), c_dec = LD((uint32_t)r0 - 0x10);
                    int32_t sb_ = abs32(fpv);
                    int32_t sl2 = (int32_t)((uint32_t)env - (uint32_t)sb_);
                    int32_t lr2 = (int32_t)((uint32_t)sb_ - (uint32_t)env);
                    int32_t env_new;
                    if (lr2 > 0){ smull(c_dec, lr2, &lo, &hi); env_new = (int32_t)((uint32_t)sb_ - (uint32_t)lsl(hi, 1)); }
                    else        { smull(c_att, sl2, &lo, &hi); env_new = (int32_t)((uint32_t)sb_ + (uint32_t)lsl(hi, 1)); }
                    ST((uint32_t)r0 - 0xb8, env_new);
                    /* ---- polynomial ---- */
                    {
                        int32_t amt = LD((uint32_t)r0 + 0x28);
                        int32_t r7v;
                        if (amt != 0){ smull(env_new, amt, &lo, &hi); r7v = lsl(hi, 4); }
                        else          { r7v = env_new; }
                        {
                            int32_t ipv2 = r7v, sb2, r6v, r1v, mx;
                            smull(0x40000000, r7v, &lo, &hi); r7v = lsl(hi, 6);
                            sb2 = LD((uint32_t)r0 - 8);
                            r7v = clamp_pos(r7v, ipv2);
                            smull(sb2, r7v, &lo, &hi); r7v = lsl(hi, 1);
                            smull(r7v, r7v, &lo, &hi); r6v = lsl(hi, 6);
                            smull(sb2, r6v, &lo, &hi); r7v = lsl(hi, 6);
                            r7v = (int32_t)(0x2d70a3d - (uint32_t)r7v);
                            smull(0x28f5c29, r7v, &lo, &hi); r7v = lsl(hi, 1);
                            r1v = (int32_t)((uint32_t)sb2 + (uint32_t)r7v);
                            mx = LD((uint32_t)r0 + 0x60);
                            if (r1v > mx) r1v = mx;
                            ST((uint32_t)r0 - 8, r1v);
                            smull(fpv, r1v, &lo, &hi); r1v = lsl(hi, 6);
                            r8 = LD((uint32_t)r0 + 0x18);
                            smull(0x40000000, r8, &lo, &hi); r1v = (int32_t)((uint32_t)r1v + (uint32_t)lsl(hi, 1));
                            smull(LD((uint32_t)r0 - 0xc4), r1v, &lo, &hi); r1v = lsl(hi, 1);
                            {
                                int32_t ip2v = LD((uint32_t)r0 + 0x68), sl3 = LD((uint32_t)r0 + 0x64);
                                r3 = (int32_t)((uint32_t)r1v + (uint32_t)r3);
                                r4 = (int32_t)((uint32_t)r1v + (uint32_t)r4);
                                r5 = r3; r6 = r4;
                                int32_t dry_l, dry_r;   /* push {r5,r6} 等价 */
                                /* optional differential filter */
                                if (!(ip2v == 1 && sl3 == 0)){
                                    int32_t P = LD((uint32_t)r0 - 0x24);
                                    int32_t k0 = LD((uint32_t)P), k1 = LD((uint32_t)P+4), k2 = LD((uint32_t)P+8);
                                    int32_t k3 = LD((uint32_t)P+0xc), k4 = LD((uint32_t)P+0x10), aa, bb, c_;
                                    smull(k0, r5, &lo, &hi); r1v = (int32_t)((uint32_t)k3 + (uint32_t)lsl(hi, 2));
                                    smull(k1, r1v, &lo, &hi);
                                    smlal(&lo, &hi, r5, k2); aa = lsl(hi, 2);
                                    smull(k0, r6, &lo, &hi);
                                    { int32_t fpv3 = (int32_t)((uint32_t)k4 + (uint32_t)lsl(hi, 2));
                                      smull(k1, fpv3, &lo, &hi);
                                      smlal(&lo, &hi, r6, k2); bb = lsl(hi, 2);
                                      c_ = LD((uint32_t)r0 - 0xd4);
                                      smull(r1v, c_, &lo, &hi); r5 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(hi, 1));
                                      smull(fpv3, c_, &lo, &hi); r6 = (int32_t)((uint32_t)r4 + (uint32_t)lsl(hi, 1));
                                    }
                                    ST((uint32_t)P + 0xc, aa); ST((uint32_t)P + 0x10, bb);
                                }
                                dry_l = r5; dry_r = r6;   /* 保存差分滤波输出（干信号） */
                                /* ---- center/width biquad ---- */
                                {
                                    int32_t B = LD((uint32_t)r0 + 0x5c);
                                    int32_t m0 = LD((uint32_t)B), m1 = LD((uint32_t)B+4), m2 = LD((uint32_t)B+8);
                                    int32_t m3 = LD((uint32_t)B+0xc), m4 = LD((uint32_t)B+0x10);
                                    int32_t m5 = LD((uint32_t)B+0x14), m6 = LD((uint32_t)B+0x18);
                                    int32_t r2v, aa;
                                    smull(m0, r3, &lo, &hi); r2v = (int32_t)((uint32_t)m5 + (uint32_t)lsl(hi, 2));
                                    smull(r2v, m1, &lo, &hi);
                                    smlal(&lo, &hi, r3, m2); aa = hi;
                                    m5 = (int32_t)((uint32_t)m6 + (uint32_t)lsl(aa, 2)); ST((uint32_t)B+0x14, m5);
                                    smull(r2v, m3, &lo, &hi);
                                    smlal(&lo, &hi, r3, m4); m6 = lsl(hi, 2);
                                    B = LD((uint32_t)r0 + 0x5c);
                                    {
                                        int32_t n0 = LD((uint32_t)B), n1 = LD((uint32_t)B+4), n2 = LD((uint32_t)B+8);
                                        int32_t n3 = LD((uint32_t)B+0xc), n4 = LD((uint32_t)B+0x10);
                                        int32_t n5 = LD((uint32_t)B+0x1c), n6 = LD((uint32_t)B+0x20);
                                        int32_t r5v2;
                                        ST((uint32_t)B+0x18, m6);
                                        smull(n0, r4, &lo, &hi); r5v2 = (int32_t)((uint32_t)n5 + (uint32_t)lsl(hi, 2));
                                        smull(r5v2, n1, &lo, &hi);
                                        smlal(&lo, &hi, r4, n2); aa = hi;
                                        n5 = (int32_t)((uint32_t)n6 + (uint32_t)lsl(aa, 2)); ST((uint32_t)B+0x1c, n5);
                                        smull(r5v2, n3, &lo, &hi);
                                        smlal(&lo, &hi, r4, n4); n6 = lsl(hi, 2);
                                        ST((uint32_t)B+0x20, n6);
                                        r3 = dry_l; r4 = dry_r;   /* pop {r3,r4} 等价 */
                                        /* ---- width ---- */
                                        {
                                            int32_t r8v = LD((uint32_t)r0 + 0x70);
                                            int32_t r6v2, r5v3;
                                            smull(r5v2, r8v, &lo, &hi); r6v2 = (int32_t)((uint32_t)r4 + (uint32_t)lsl(hi, 4));
                                            smull(r2v, r8v, &lo, &hi); r5v3 = (int32_t)((uint32_t)r3 + (uint32_t)lsl(hi, 4));
                                            {
                                                int32_t sb3 = LD((uint32_t)r0 + 0x1c);
                                                int32_t r5v4, r6v3;
                                                if (sb3 != 1){ r5v4 = r5v3; r6v3 = r6v2; }
                                                else {
                                                    uint32_t r1v2 = (uint32_t)r0 - 0xe0;
                                                    int32_t sb4 = r5v3, ipv9 = r6v2;
                                                    int32_t g = LD(r1v2 + 0x10c);
                                                    int32_t r2v2, r3v2, r7v2, r8v2;
                                                    smull(r5v3, g, &lo, &hi); r2v2 = lsl(hi, 2);
                                                    smull(r6v2, g, &lo, &hi); r3v2 = lsl(hi, 2);
                                                    r7v2 = abs32(r2v2); r8v2 = abs32(r3v2);
                                                    if (r7v2 < r8v2) r7v2 = r8v2;
                                                    if (r7v2 > 0x2000000){
                                                        int32_t r5v5, fpv5;
                                                        r7v2 = (int32_t)((uint32_t)r7v2 - 0x2000000);
                                                        smull(r7v2, 0x1999999a, &lo, &hi); r7v2 = lsl(hi, 5);
                                                        r7v2 = (int32_t)(0x7FFFFFFF - (uint32_t)r7v2);
                                                        smull(r7v2, LD(r1v2 + 0x10c), &lo, &hi); r5v5 = lsl(hi, 1);
                                                        smull(LD(r1v2 + 0x100), (int32_t)0xd0000000, &lo, &hi);
                                                        fpv5 = (int32_t)((uint32_t)lsl(hi, 1) + 0x40000000);
                                                        if (r5v5 < fpv5) r5v5 = fpv5;
                                                        ST(r1v2 + 0x10c, r5v5);
                                                        smull(sb4, r5v5, &lo, &hi); r2v2 = lsl(hi, 2);
                                                        smull(ipv9, r5v5, &lo, &hi); r3v2 = lsl(hi, 2);
                                                        smull(r2v2, 0x40000000, &lo, &hi); r5v4 = lsl(hi, 4);
                                                        smull(r3v2, 0x40000000, &lo, &hi); r6v3 = lsl(hi, 4);
                                                    } else {
                                                        if (g >= 0x40000000){
                                                            smull(r2v2, 0x40000000, &lo, &hi); r5v4 = lsl(hi, 4);
                                                            smull(r3v2, 0x40000000, &lo, &hi); r6v3 = lsl(hi, 4);
                                                        } else {
                                                            int32_t fpv8;
                                                            smull(LD(r1v2 + 0x104), g, &lo, &hi); fpv8 = lsl(hi, 1);
                                                            g = (int32_t)((uint32_t)g + (uint32_t)fpv8);
                                                            if (g > 0x40000000) g = 0x40000000;
                                                            ST(r1v2 + 0x10c, g);
                                                            smull(r2v2, 0x40000000, &lo, &hi); r5v4 = lsl(hi, 4);
                                                            smull(r3v2, 0x40000000, &lo, &hi); r6v3 = lsl(hi, 4);
                                                        }
                                                    }
                                                }
                                                /* ---- output ---- */
                                                {
                                                    int32_t r8v2 = LD((uint32_t)r0 + 0x34);
                                                    int32_t r2v3, r5v5b, fpv9, r7v3;
                                                    (void)fpv9; (void)r7v3; /* transcription residue */
                                                    smull(r5v4, r8v2, &lo, &hi); r5v4 = lsl(hi, 2);
                                                    smull(r6v3, r8v2, &lo, &hi); r6v3 = lsl(hi, 2);
                                                    r1 = LD((uint32_t)r0);
                                                    r3 = LD((uint32_t)r0 + 4);
                                                    /* v2.1: asr r2,r2,#0x10 硬编码 */
                                                    r2v3 = sat_out(r5v4, 0x10);
                                                    r5v5b = sat_out(r6v3, 0x10);
                                                    ST((uint32_t)r1, r2v3); ST((uint32_t)r1 + 4, r5v5b);
                                                    r1 = (int32_t)((uint32_t)r1 + 8);
                                                    r3 = (int32_t)((uint32_t)r3 - 1);
                                                    if (r3 == 0) return;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
