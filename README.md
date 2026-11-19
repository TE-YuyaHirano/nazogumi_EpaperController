# nazogumi EpaperController

M5Stack Basic と Raspberry Pi Zero PhotoPainter 基板を使い、7.3インチ6色電子ペーパーへ目の画像を表示するコントローラーです。

## 主な機能

- 起動時の現在時刻設定UI
- 設定した開始時刻に開いた目、終了時刻に閉じた目を表示
- A/Cボタンによる手動の閉眼・開眼表示
- SDカードの画像と設定ファイルに対応
- 内蔵画像へのフォールバック
- M5Basicの上下逆取り付けに対応した画面・ボタン配置

## ディレクトリ

- `firmware/susuki_epaper_button_test/`: Arduinoファームウェア、画像、変換ツール、SDカード用サンプル
- `hardware/m5basic_spacers/`: 基板貼り付け用スペーサーのSTLと生成スクリプト

## SDカード

SDカード直下へ次のように配置します。

```text
/setting.txt
/assets/me_close.jpg
/assets/me_open.jpg
```

`setting.txt` の例：

```text
INITIAL_TIME=12:00
START_TIME=18:00
END_TIME=09:00
```

キー名は大文字・小文字のどちらでも読み取れます。

## Arduino

対象スケッチ：

`firmware/susuki_epaper_button_test/susuki_epaper_button_test.ino`

対象ボード：M5Stack Basic（ESP32）

