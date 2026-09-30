# Capture Calibration 実機検証結果

## 測定条件

- 測定日：2026-09-29
- Android端末：P30T
- テストパターン：Capture Calibration 24パッチ（6列×4行）
- 測定ROI：各パッチ中央50%
- Transport Decode：BT.601 Limited Range YCbCrからEncoded RGBへ変換
- Validation測定：Settling 4フレーム後、16フレームを測定
- Validation閾値は暫定値であり、製品要件として確定したものではない
- HDMI送信側の出力設定は今回の測定記録に含まれていない

## 測定結果

| 評価項目 | MS2109S | MS2130 | MS2109 |
|---|---:|---:|---:|
| USB識別情報 | `345F:2109` | `345F:2130` | `534D:2109` |
| Product文字列 | Android上では`/dev/bus/usb/001/002`と表示 | `USB3.0 UHD` | `USB Video` |
| 補正前MAE | 3.1053 | 0.7729 | 2.0600 |
| 補正後MAE | 3.5307 | 0.9118 | 2.5461 |
| 補正前RMSE | 5.8724 | 1.3949 | 3.7428 |
| 補正後RMSE | 5.1205 | 1.1538 | 3.3323 |
| 最大チャンネル誤差 | 23.5292 | 3.1779 | 9.7455 |
| Neutral Axis RMSE | 5.0443 | 1.4286 | 4.3976 |
| Primary RMSE | 5.5997 | 0.8929 | 2.8943 |
| Secondary RMSE | 4.4359 | 1.0333 | 1.6858 |
| Black Offset最大値 | 5.0171 | 1.5011 | 3.1618 |
| White誤差最大値 | 1.6674 | 1.2264 | 1.5832 |
| Clipping検出 | なし | なし | なし |
| Matrix条件数 | 3.8186 | 3.7843 | 3.7616 |
| Validation結果 | `CALIBRATION_POOR_FIT` | `CALIBRATION_VALID` | `CALIBRATION_POOR_FIT` |

## MS2109S所見

- 結果スクリーンショット：`phase6_validation_bt601_limited.png`
- Matrix補正によりRMSEは5.8724から5.1205へ低下した
- MAEは3.1053から3.5307へ増加した
- 補正後MAE、最大チャンネル誤差、Neutral Axis RMSEが暫定基準を超過した
- Matrix条件数は良好であり、Clippingも検出されていない
- Singular Solveではなく、補正モデルのFit品質に関する問題と判断する

## MS2130所見

- 結果スクリーンショット：`phase6_ms2130.png`
- Matrix補正によりRMSEは1.3949から1.1538へ低下した
- MAEは0.7729から0.9118へわずかに増加したが、暫定基準内である
- 現在適用しているすべてのValidation基準を通過した
- Matrix条件数は良好であり、Clippingも検出されていない

## MS2109所見

- 結果スクリーンショット：`phase6_ms2109.png`
- Matrix補正によりRMSEは3.7428から3.3323へ低下した
- MAEは2.0600から2.5461へ増加したが、暫定基準内である
- 最大チャンネル誤差、Black Offset、White誤差、Matrix条件数は基準を通過した
- 強制判定項目のうち、Neutral Axis RMSEのみが暫定基準を超過した
  - 測定値：4.3976
  - 暫定基準：4.0以下
- MS2109Sより大幅に良好であり、現在の`CALIBRATION_VALID`境界に近い結果である

## 現在の暫定Validation基準

```text
補正後MAE             3以下
最大チャンネル誤差   12以下
Neutral Axis RMSE     4以下
Black Offset最大値    6以下
White誤差最大値      10以下
Matrix条件数       10000以下
補正後RMSEが補正前RMSEより小さいこと
```
