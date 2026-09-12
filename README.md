# zmk-feature-battery-monitor

ZMK 用の再利用可能な battery sensor driver です。ADC で得た電圧を抵抗分圧から復元し、EMA（指数移動平均）で平滑化してから、Devicetree で指定した電圧→残量カーブに従って 0〜100% の残量を返します。

このモジュールは battery chemistry、低電圧停止、充電制御、スリープ制御を扱いません。各キーボードの回路と電池に合わせる値は、すべて Devicetree で指定します。

## ZMK への追加

`config/west.yml` の `projects` にモジュールを追加します。

```yaml
manifest:
  projects:
    - name: zmk-feature-battery-monitor
      remote: nixiy
      revision: main
      import: app.yml
  remotes:
    - name: nixiy
      url-base: https://github.com/nixiy
```

既存の `remotes` 定義を使う場合は、それに合わせてください。更新後に `west update zmk-feature-battery-monitor` を実行します。

## Devicetree 使用例

キーボードの `.dts` または `.overlay` に ADC ノードと battery sensor を定義し、ZMK の `chosen` battery をこのノードに向けます。

```dts
/ {
    chosen {
        zmk,battery = &battery;
    };

    battery: battery {
        compatible = "nixiy,battery-monitor";

        io-channels = <&adc 0>;

        output-ohms = <470000>;
        full-ohms = <1470000>;

        mv-to-pct-thresholds =
            <1000 1120 1170 1190 1210
             1230 1250 1270 1290 1330 1400>;

        smoothing-factor = <8>;
        battery-replacement-threshold-mv = <150>;
    };
};
```

上記の抵抗値とカーブは一例です。Nickey44A など特定のキーボードへ導入する際も、driver を変えずに、その回路・電池向けの値を overlay に置いてください。

ADC 分圧回路を GPIO で給電する場合は、次の property も加えられます。

```dts
power-gpios = <&gpio0 15 GPIO_ACTIVE_HIGH>;
```

この指定があると、driver は測定ごとに GPIO を ON にして 10 ms 待ち、ADC を読んだ後に GPIO を OFF にします。

## Properties

| Property | Required | Meaning |
| --- | --- | --- |
| `io-channels` | Yes | ADC input。`<&adc channel>` の形式です。 |
| `output-ohms` | Yes | 分圧回路の ADC/GND 側の抵抗値（Ω）。 |
| `full-ohms` | Yes | 分圧回路全体の抵抗値（Ω）。 |
| `mv-to-pct-thresholds` | Yes | 0% から 100% まで等間隔の電圧しきい値配列（mV）。昇順で指定します。 |
| `smoothing-factor` | No | EMA の係数。既定値は `8`。`0` と `1` は平滑化なしとして扱います。 |
| `battery-replacement-threshold-mv` | No | EMA をリセットする上方向の電圧ジャンプ（mV）。既定値 `0` は無効です。 |
| `power-gpios` | No | ADC 分圧回路を測定時だけ給電する GPIO。 |

バッテリー電圧は次式で復元します。

```text
battery_mv = adc_mv * full_ohms / output_ohms
```

`mv-to-pct-thresholds` の要素は 0〜100% を等間隔で表します。たとえば `<1000 1100 1200>` なら、1000 mV は 0%、1100 mV は 50%、1200 mV は 100% で、間は線形補間されます。最初の値未満は 0%、最後の値以上は 100% です。

## EMA smoothing

初回測定はそのまま filter の値になります。以降、`N = smoothing-factor` として次式（整数の round-to-nearest）を使います。

```text
filtered = (filtered * (N - 1) + measured + N / 2) / N
```

たとえば `smoothing-factor = <8>` は新しい測定値を 1/8 だけ反映します。`<1>`（または安全のため `<0>`）なら毎回 `filtered = measured` となります。

## Battery replacement detection

`battery-replacement-threshold-mv` が 0 より大きく、初回測定後に次の条件を満たした場合、電池交換とみなして EMA を即時リセットします。

```text
measured_mv >= filtered_mv + threshold
```

下方向の急変は検出せず、通常どおり EMA に通します。この機能は充電状態の判定には使いません。電池交換を必要とする構成だけで有効にしてください。

## Sensor values

標準 ZMK/Zephyr sensor API の次の channel を提供します。

- `SENSOR_CHAN_GAUGE_VOLTAGE`: 平滑化後の電圧を volts + microvolts 形式で返します。
- `SENSOR_CHAN_GAUGE_STATE_OF_CHARGE`: 平滑化後の電圧から計算した整数の 0〜100% を返します。
- `SENSOR_CHAN_ALL`: fetch 時に両方の値を更新します。
