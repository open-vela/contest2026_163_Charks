/*
 * R528 audiocodec 采集输入通路的使能。
 *
 * 为什么必须有这个文件（这是本项目踩得最深的一个坑）
 * ------------------------------------------------
 * 2026-09-16 实测：**每次开机后，混音器的麦克风输入开关默认全是 Off**，
 * 此时任何采集都会在 hw_params 阶段失败，并报一条与真实原因
 * **完全不符**的错误：
 *
 *     [SND_ERR][sunxi_codec_hw_params:1250] capture only support 1~3 channel
 *     [SND_ERR][soc_pcm_hw_params:423]      codec_dai set hw_params failed
 *
 * 真实原因是驱动 sunxi_get_adc_ch() 去读 codec 的模拟寄存器、判断哪条
 * 输入通路（MIC PGA / FM IN / LINE IN）被使能；一条都没使能就返回 -1。
 * 那条报错消息写错了，与"声道数"毫无关系（实测 -c 1/2/3 报的是同一个错）。
 *
 * ⚠️ 别去调声道数。上一版就是因为信了这条消息，绕了很久。
 *
 * 而且**这个设置不持久化**，reboot 之后又全回到 Off。所以必须在程序里做，
 * 不能靠人工敲 amixer —— 否则就是"有时能用有时不能用"的间歇故障。
 *
 * 增益也在这里一并处理：厂商默认 ADC 数字增益 160，比实测可用点低约 20dB，
 * 不抬起来录到的声音会很轻，直接拖低云端 ASR 的识别率。
 *
 * 依赖：aw-alsa-lib/control.h（板子已有的 ALSA control 接口，amixer 用的就是它）
 */

#ifndef __SUPERCHILD_SC_MICROUTE_H
#define __SUPERCHILD_SC_MICROUTE_H

/*
 * 打开采集所需的全部混音器开关并补足增益。
 *
 * 幂等，带回读确认（驱动可能接受了调用但不生效）。
 * 必须在**打开采集设备之前**调用。
 *
 * @return 0 全部就绪；
 *         -EIO 有任一项失败（此时采集很可能报那条"1~3 channel"的误导错误）；
 *         -EINVAL 参数错误。
 *
 * 注意：失败**不应该致命**。板子上的控制项名称若与固件版本不同，
 * 本函数会打出具体是哪个名称找不到 —— 照提示改 sc_microute.c 里的表即可。
 */
int sc_microute_enable(void);

/* 只补增益（不碰开关）。sc_microute_enable() 内部会调，通常不用单独调。 */
int sc_microute_apply_gain(void);

/* 读某个控制项当前值，用于诊断。 */
int sc_microute_query(const char *name, unsigned long *value);

#endif /* __SUPERCHILD_SC_MICROUTE_H */
