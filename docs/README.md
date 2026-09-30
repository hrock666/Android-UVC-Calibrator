# Documentation

Android UVC Calibratorの設計、Profile仕様、検証記録をまとめています。

## 現行ドキュメント

| 文書 | 内容 |
|---|---|
| [Architecture](Architecture.md) | 現在の実装構成、処理フロー、スレッド境界、データの流れ |
| [Calibration Profile Format v1](Capture_Calibration_Profile_Format_v1.md) | JSON Profileのフィールド、照合条件、保存仕様 |
| [Validation Results](Capture_Calibration_Validation_Results.md) | 実機検証結果と既知の傾向 |

## 設計経緯・実装計画

以下は設計判断とPhase実装の履歴を残す資料です。現行挙動の入口には、上記の
Architectureとルートの[README](../README.md)を使用してください。

| 文書 | 内容 |
|---|---|
| [Integrated Architecture Design Specification](Capture_Calibration_Integrated_Architecture_Design_Specification.md) | 詳細設計と机上検討 |
| [Implementation Plan Phase 0–11](Capture_Calibration_Implementation_Plan_Phase0-11.md) | Phaseごとの実装計画と判断記録 |

## 画像

`image/`にはREADMEおよび検証資料から参照する画面キャプチャを格納します。
画像を追加するときは、参照元の文書と用途が分かる名前にしてください。
