/* SWD (SWD) DSP core —— 0x1000b000 定点 C 实现（与已验证 Python 等价） */
#ifndef SWD21_H
#define SWD21_H

#include <stdint.h>

/* 用户实现：把 32 位地址映射为主机指针（测试时用区域表；MCU 上可身份映射） */
uint8_t *swd_map(uint32_t addr);

/* state: DSP 状态结构地址（含指向滤波器实例的 32 位指针字段）
 * buf:   int32 样本缓冲地址（L,R 交错，每样本 4 字节），需 count*8 字节
 */
void swd21_process(uint32_t state, uint32_t buf);

/* 系数装载器（驱动 0x1000832c）：
 * 在改变 state[0x10] (sel/TB尺寸) 或 state[0x14] 后必须调用，
 * 否则 DSP 用的滤波器实例指针仍指向旧数据。 */
void swd21_coef_load(uint32_t state);

#endif
