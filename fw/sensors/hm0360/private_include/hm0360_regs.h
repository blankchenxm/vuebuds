/*
 * HM0360 register map (datasheet Preliminary V04; names follow the OpenMV driver).
 */
#ifndef HM0360_REGS_H
#define HM0360_REGS_H

/* Sensor identification (read-only). */
#define HM0360_REG_MODEL_ID_H               0x0000U
#define HM0360_REG_MODEL_ID_L               0x0001U
#define HM0360_REG_SILICON_REV              0x0002U
#define HM0360_REG_FRAME_COUNT_H            0x0005U
#define HM0360_REG_FRAME_COUNT_L            0x0006U

/* Sensor mode control. */
#define HM0360_REG_MODE_SELECT              0x0100U
#define HM0360_REG_IMAGE_ORIENTATION        0x0101U
#define HM0360_REG_SW_RESET                 0x0103U
#define HM0360_REG_COMMAND_UPDATE           0x0104U  // latches CMU (double-buffered) registers

#define HM0360_MODE_SLEEP                   0x00U    // "Sleep1": software standby
#define HM0360_MODE_STREAMING               0x01U
#define HM0360_MODE_AUTO_WAKEUP             0x02U    // Streaming 2 (S1)
#define HM0360_MODE_SNAPSHOT                0x03U
#define HM0360_SOFTWARE_RESET               0x01U
#define HM0360_COMMAND_UPDATE_APPLY         0x01U

/* Exposure and gain. */
#define HM0360_REG_INTEGRATION_H            0x0202U
#define HM0360_REG_INTEGRATION_L            0x0203U
#define HM0360_REG_ANALOG_GAIN              0x0205U
#define HM0360_REG_DIGITAL_GAIN_H           0x020EU
#define HM0360_REG_DIGITAL_GAIN_L           0x020FU

/* Clock control. PLL1CFG [5:4] I2C div, [3:2] PCLKO div, [1:0] CLK_TB (Sensor_Core) div. */
#define HM0360_REG_PLL1_CONFIG              0x0300U
#define HM0360_REG_PLL2_CONFIG              0x0301U
#define HM0360_REG_PLL3_CONFIG              0x0302U
#define HM0360_PLL1_I2C_DIV2                0x00U
#define HM0360_PLL1_PCLKO_DIV1              0x04U
#define HM0360_PLL1_CORE_DIV8               0x03U

/* Frame timing. */
#define HM0360_REG_FRAME_LENGTH_LINES_H     0x0340U
#define HM0360_REG_FRAME_LENGTH_LINES_L     0x0341U
#define HM0360_REG_LINE_LENGTH_PCK_H        0x0342U
#define HM0360_REG_LINE_LENGTH_PCK_L        0x0343U

/* Monochrome. */
#define HM0360_REG_MONO_MODE                0x0370U
#define HM0360_REG_MONO_MODE_ISP            0x0371U
#define HM0360_REG_MONO_MODE_SEL            0x0372U

/* Sub-sampling / binning: SUB 0 = full, 1 = Sub2, 2 = Sub4; BINNING [1] H, [0] V. */
#define HM0360_REG_H_SUBSAMPLE              0x0380U
#define HM0360_REG_V_SUBSAMPLE              0x0381U
#define HM0360_REG_BINNING_MODE             0x0382U
#define HM0360_SUB_FULL                     0x00U
#define HM0360_SUB_2                        0x01U
#define HM0360_SUB_4                        0x02U
#define HM0360_BINNING_HV                   0x03U
#define HM0360_BINNING_OFF                  0x00U

/* Test pattern: [6:4] mode, [0] enable. */
#define HM0360_REG_TEST_PATTERN_MODE        0x0601U
#define HM0360_TEST_PATTERN_DISABLED        0x00U
#define HM0360_TEST_PATTERN_COLOR_BAR       0x01U
#define HM0360_TEST_PATTERN_WALKING_1       0x21U

/* Black level. */
#define HM0360_REG_BLC_TGT                  0x1004U
#define HM0360_REG_BLC2_TGT                 0x1009U
#define HM0360_REG_MONO_CTRL                0x100AU

/* Output format. [3] PCLKO gating enable, [1] HSYNC shift, [0] VSYNC shift. */
#define HM0360_REG_OPFM_CTRL                0x1014U
#define HM0360_OPFM_PCLKO_GATED             0x08U

/* Tone mapping. */
#define HM0360_REG_CMPRS_CTRL               0x102FU
#define HM0360_REG_CMPRS_01                 0x1030U

/* Automatic exposure. */
#define HM0360_REG_AE_CTRL                  0x2000U
#define HM0360_REG_AE_CTRL1                 0x2001U
#define HM0360_REG_MAX_INTG_H               0x2029U
#define HM0360_REG_MAX_INTG_L               0x202AU
#define HM0360_REG_MAX_AGAIN                0x202BU
#define HM0360_REG_MAX_DGAIN_H              0x202CU
#define HM0360_REG_MAX_DGAIN_L              0x202DU
#define HM0360_REG_T_DAMPING                0x2031U
#define HM0360_REG_N_DAMPING                0x2032U
#define HM0360_REG_AE_TARGET_MEAN           0x2034U
#define HM0360_REG_AE_MIN_MEAN              0x2035U
#define HM0360_REG_AE_TARGET_ZONE           0x2036U
#define HM0360_REG_CONVERGE_IN_TH           0x2037U
#define HM0360_REG_CONVERGE_OUT_TH          0x2038U

/* Interrupt and motion detection (both unused; MD is switched off). */
#define HM0360_REG_PULSE_MODE               0x2061U
#define HM0360_REG_MD_CTRL                  0x2080U
#define HM0360_REG_MD_CTRL1                 0x209EU

/* Context switch: [3] context disable, [2] auto, [1] CXT_SEL pin select. */
#define HM0360_REG_PMU_CFG_3                0x3024U
#define HM0360_PMU_CFG_3_CONTEXT_DISABLE    0x08U

/* 0x3030[0]: 0 = 656 x 496 readout, 1 = 640 x 480 window (first pixel at (8, 8)). */
#define HM0360_REG_WIN_MODE                 0x3030U
#define HM0360_WIN_MODE_640X480             0x01U

/* Output enable: [2] PCLKO continuous mode (overrides gating), [1] trigger on/off, [0] VSYNC mode. */
#define HM0360_REG_OUTPUT_EN                0x30A5U
#define HM0360_OUTPUT_EN_PCLKO_CONTINUOUS   0x04U
#define HM0360_OUTPUT_EN_VSYNC_MODE         0x01U

/* PCLKO gating: [1] gated by line, [0] gated by frame. */
#define HM0360_REG_PCLKO_GATED_EN           0x309EU
#define HM0360_PCLKO_GATED_MASK             0x03U
#define HM0360_PCLKO_GATED_BY_LINE          0x02U

/* Extra PCLKO clocks before / after each gated line (16-bit, H/L). */
#define HM0360_REG_PCLKO_LINE_FRONT_H       0x30A1U
#define HM0360_REG_PCLKO_LINE_FRONT_L       0x30A2U
#define HM0360_REG_PCLKO_LINE_END_H         0x30A3U
#define HM0360_REG_PCLKO_LINE_END_L         0x30A4U

/* Data width: 0x310F [7] 1-bit enable, [6] 4-bit enable (both 0 = 8-bit). */
#define HM0360_REG_ANA_REGISTER_04          0x310FU
#define HM0360_SERIAL_WIDTH_MASK            0xC0U
#define HM0360_SERIAL_1BIT                  0x80U

/* 0x3112 [3] MSB first (1-bit / 4-bit), [2] PCLKO polarity. */
#define HM0360_REG_ANA_REGISTER_07          0x3112U
#define HM0360_MSB_FIRST                    0x08U
#define HM0360_PCLKO_POLARITY_MASK          0x04U
// PCLKO edge the data changes on relative to SPIS mode 0 (samples on rising SCK).
#ifndef HM0360_PCLKO_POLARITY
#define HM0360_PCLKO_POLARITY               0x00U
#endif

#endif /* HM0360_REGS_H */
