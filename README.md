# Charge Monitor – TrimUI Brick Pro (stock OS)

Ứng dụng theo dõi quá trình sạc pin và tối ưu máy trong lúc sạc cho **TrimUI Brick Pro** chạy firmware gốc (stock OS). Phiên bản hiện tại: **v1.3**.

## Tính năng

- Hiển thị % pin, trạng thái sạc và thông số sạc trên màn hình.
- Tối ưu khi sạc (tự khôi phục khi thoát):
  - CPU về xung thấp nhất, tắt nhân 1–3
  - Giảm đèn nền ~12%, tắt đèn LED RGB
  - Tắt Wi-Fi (bật lại khi thoát nếu trước đó đang bật)
- Màn hình tự tắt sau 30 giây không bấm phím.
- Tuỳ chọn **tự tắt máy khi pin đầy**.
- Ghi log sạc ra file CSV mỗi 30 giây.

## Cài đặt

1. Tải repo này (Code → Download ZIP) hoặc bản phát hành.
2. Chép thư mục `Apps/ChargeMonitor` vào thẻ nhớ: `SD:/Apps/ChargeMonitor/` (gộp thư mục nếu được hỏi).
3. Mở menu **Apps → Charge Monitor**, rồi cắm sạc.

## Phím

| Phím | Chức năng |
|---|---|
| START | Tắt màn hình ngay |
| SELECT | Bật/tắt "tự tắt máy khi pin đầy" |
| MENU / giữ 2 giây | Thoát (khôi phục mọi cài đặt) |
| Phím bất kỳ | Bật lại màn hình |

## Log

- `logs/charge_*.csv` – số liệu sạc, ghi mỗi 30 giây
- `logs/run.log` – thông tin chẩn đoán, mã phím. Nếu phím không đúng, mở file này xem dòng `key NNN`.

## Build lại từ mã nguồn

Mã nguồn nằm trong `Apps/ChargeMonitor/src/`. Build tĩnh cho aarch64 bằng [Zig](https://ziglang.org/):

```sh
cd Apps/ChargeMonitor
# (tuỳ chọn) tạo lại font.h – cần Python 3 + Pillow và font DejaVu Sans
python3 src/gen_font.py
zig cc -target aarch64-linux-musl -O2 -static -o chargemon src/chargemon.c -lm
```

## Ghi chú

- Font chữ hiển thị được sinh từ DejaVu Sans (giấy phép DejaVu Fonts License).
- App chỉ dành cho TrimUI Brick Pro chạy stock OS; chưa thử trên firmware khác.
