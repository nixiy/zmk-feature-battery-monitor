/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT nixiy_battery_monitor

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_ADC_NRFX_SAADC
#include <hal/nrf_saadc.h>
#endif

LOG_MODULE_REGISTER(zmk_feature_battery_monitor, CONFIG_ZMK_LOG_LEVEL);

struct battery_monitor_config {
    /* Devicetree から取得する、回路・電池ごとの設定値。 */
    uint8_t adc_channel;
    struct gpio_dt_spec power;
    uint32_t output_ohms;
    uint32_t full_ohms;
    const uint16_t *mv_to_pct_thresholds;
    uint8_t mv_to_pct_thresholds_size;
    uint32_t smoothing_factor;
    uint32_t battery_replacement_threshold_mv;
};

struct battery_monitor_data {
    /* 実行時に更新される ADC とフィルタの状態。 */
    const struct device *adc;
    struct adc_channel_cfg acc;
    struct adc_sequence as;

    int16_t adc_raw;
    uint16_t measured_mv;
    uint16_t filtered_mv;
    uint8_t state_of_charge;
    bool filter_initialized;
};

static uint8_t mv_to_pct_linear_interpolation(uint16_t millivolts,
                                               const uint16_t *thresholds, uint8_t size) {
    /* 配列の先頭・末尾は、それぞれ 0%・100% として扱う。 */
    if (size < 2 || millivolts <= thresholds[0]) {
        return 0;
    }

    if (millivolts >= thresholds[size - 1]) {
        return 100;
    }

    for (uint8_t i = 1; i < size; i++) {
        uint16_t high = thresholds[i];
        uint16_t low = thresholds[i - 1];

        if (millivolts < high) {
            /*
             * 昇順でない配列でもゼロ除算しないよう、その区間の上端の
             * 残量を返す。通常は Devicetree で昇順に指定する。
             */
            if (high <= low) {
                return (uint8_t)(((uint32_t)i * 100U) / (size - 1U));
            }

            /*
             * 各要素が表す残量間隔は 100 / (size - 1) %。
             * 浮動小数点を使わず、この区間内を線形補間する。
             */
            uint32_t numerator = ((uint32_t)(i - 1U) * 100U * (high - low)) +
                                 ((uint32_t)(millivolts - low) * 100U);
            uint32_t denominator = (uint32_t)(size - 1U) * (high - low);

            return (uint8_t)(numerator / denominator);
        }
    }

    return 100;
}

static int battery_monitor_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct battery_monitor_data *data = dev->data;
    const struct battery_monitor_config *config = dev->config;
    int rc;

    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    /* 分圧回路を必要な測定時だけ給電し、電圧が安定するまで待つ。 */
    rc = gpio_pin_set_dt(&config->power, 1);
    if (rc != 0) {
        LOG_DBG("Failed to enable ADC power GPIO: %d", rc);
        return rc;
    }

    k_sleep(K_MSEC(10));
#endif

    /* 最初の測定だけ ADC 校正を実施する。 */
    rc = adc_read(data->adc, &data->as);
    data->as.calibrate = false;

    if (rc == 0) {
        int32_t adc_mv = data->adc_raw;

        /* ADC の生値を ADC 入力電圧（mV）へ変換する。 */
        rc = adc_raw_to_millivolts(adc_ref_internal(data->adc), data->acc.gain,
                                   data->as.resolution, &adc_mv);
        if (rc == 0 && adc_mv >= 0) {
            /*
             * 分圧された ADC 電圧を、本来の電池電圧へ復元する。
             * 中間値は抵抗値との積で大きくなり得るため 64 bit を使う。
             */
            uint64_t battery_mv = (uint64_t)adc_mv * config->full_ohms / config->output_ohms;

            data->measured_mv = MIN(battery_mv, UINT16_MAX);

            if (!data->filter_initialized) {
                /* 初回には過去値がないため、測定値をそのまま採用する。 */
                data->filtered_mv = data->measured_mv;
                data->filter_initialized = true;
            } else if (config->battery_replacement_threshold_mv > 0U &&
                       data->measured_mv >=
                           (uint32_t)data->filtered_mv +
                               config->battery_replacement_threshold_mv) {
                /* 大きな上昇だけを電池交換と見なし、EMA の履歴を捨てる。 */
                LOG_DBG("Battery replacement detected: filtered=%umV measured=%umV",
                        data->filtered_mv, data->measured_mv);
                data->filtered_mv = data->measured_mv;
            } else if (config->smoothing_factor <= 1U) {
                /* 0 も 1 と同様に扱い、設定異常でもゼロ除算を防ぐ。 */
                data->filtered_mv = data->measured_mv;
            } else {
                uint32_t factor = config->smoothing_factor;
                /* EMA: 新しい測定値の寄与は 1 / factor。N / 2 で四捨五入する。 */
                uint64_t sum = (uint64_t)data->filtered_mv * (factor - 1U) +
                               data->measured_mv + factor / 2U;

                data->filtered_mv = (uint16_t)(sum / factor);
            }

            /* 生の測定値ではなく、平滑化後の電圧から残量を算出する。 */
            data->state_of_charge = mv_to_pct_linear_interpolation(
                data->filtered_mv, config->mv_to_pct_thresholds,
                config->mv_to_pct_thresholds_size);
            LOG_DBG("Battery raw=%d adc=%dmV measured=%umV filtered=%umV soc=%u%%",
                    data->adc_raw, adc_mv, data->measured_mv, data->filtered_mv,
                    data->state_of_charge);
        } else if (rc == 0) {
            rc = -ERANGE;
        }
    }

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    {
        /* ADC 読み取りの成否にかかわらず、分圧回路の給電を止める。 */
        int power_rc = gpio_pin_set_dt(&config->power, 0);

        if (power_rc != 0) {
            LOG_DBG("Failed to disable ADC power GPIO: %d", power_rc);
            if (rc == 0) {
                rc = power_rc;
            }
        }
    }
#endif

    if (rc != 0) {
        LOG_DBG("Failed to read ADC: %d", rc);
    }

    return rc;
}

static int battery_monitor_channel_get(const struct device *dev, enum sensor_channel chan,
                                       struct sensor_value *val) {
    const struct battery_monitor_data *data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        /* Zephyr の sensor_value は「V と µV」の組で電圧を表現する。 */
        val->val1 = data->filtered_mv / 1000U;
        val->val2 = (data->filtered_mv % 1000U) * 1000U;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        /* 残量は 0〜100 の整数値として返す。 */
        val->val1 = data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct sensor_driver_api battery_monitor_api = {
    .sample_fetch = battery_monitor_sample_fetch,
    .channel_get = battery_monitor_channel_get,
};

static int battery_monitor_init(const struct device *dev) {
    struct battery_monitor_data *data = dev->data;
    const struct battery_monitor_config *config = dev->config;
    int rc;

    if (!device_is_ready(data->adc)) {
        LOG_ERR("ADC device is not ready");
        return -ENODEV;
    }

    /* ゼロ除算や、0%/100% を作れないカーブを初期化時に拒否する。 */
    if (config->output_ohms == 0U || config->mv_to_pct_thresholds_size < 2U) {
        LOG_ERR("Invalid battery monitor Devicetree configuration");
        return -EINVAL;
    }

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    /* power-gpios は inactive 状態で初期化し、待機時の消費を抑える。 */
    if (!device_is_ready(config->power.port)) {
        LOG_ERR("GPIO port for power control is not ready");
        return -ENODEV;
    }

    rc = gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
    if (rc != 0) {
        LOG_ERR("Failed to configure ADC power GPIO: %d", rc);
        return rc;
    }
#endif

    /* ZMK 標準の battery voltage divider と同じ SAADC 測定条件。 */
    data->as = (struct adc_sequence){
        .channels = BIT(0),
        .buffer = &data->adc_raw,
        .buffer_size = sizeof(data->adc_raw),
        .oversampling = 4,
        .calibrate = true,
        .resolution = 12,
    };

#ifdef CONFIG_ADC_NRFX_SAADC
    data->acc = (struct adc_channel_cfg){
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput0 + config->adc_channel,
    };
#else
#error "battery-monitor currently requires ADC_NRFX_SAADC"
#endif

    rc = adc_channel_setup(data->adc, &data->acc);
    LOG_DBG("AIN%u setup returned %d", config->adc_channel, rc);
    return rc;
}

/* この driver は ZMK 標準 driver と同様、最初の compatible ノードを使用する。 */
static struct battery_monitor_data battery_monitor_data = {
    .adc = DEVICE_DT_GET(DT_IO_CHANNELS_CTLR(DT_DRV_INST(0))),
};
static const uint16_t battery_monitor_thresholds[] = DT_INST_PROP(0, mv_to_pct_thresholds);
static const struct battery_monitor_config battery_monitor_config = {
    .adc_channel = DT_IO_CHANNELS_INPUT(DT_DRV_INST(0)),
#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    .power = GPIO_DT_SPEC_INST_GET(0, power_gpios),
#endif
    .output_ohms = DT_INST_PROP(0, output_ohms),
    .full_ohms = DT_INST_PROP(0, full_ohms),
    .mv_to_pct_thresholds = battery_monitor_thresholds,
    .mv_to_pct_thresholds_size = DT_INST_PROP_LEN(0, mv_to_pct_thresholds),
    .smoothing_factor = DT_INST_PROP_OR(0, smoothing_factor, 8),
    .battery_replacement_threshold_mv =
        DT_INST_PROP_OR(0, battery_replacement_threshold_mv, 0),
};

DEVICE_DT_INST_DEFINE(0, battery_monitor_init, NULL, &battery_monitor_data,
                      &battery_monitor_config, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &battery_monitor_api);
