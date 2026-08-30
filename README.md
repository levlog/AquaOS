# AquaOS

Настоящая загружаемая операционная система (bootable ISO) с графической оболочкой в стиле macOS. Не симуляция: внутри — ядро Linux 6.12 (Debian), собственный init (PID 1) и самописный графический композитор, рисующий напрямую во фреймбуфер `/dev/fb0`.

## Что на экране

- **Загрузка**: в нижнем левом углу бегут реальные логи ядра (читаются из `/dev/kmsg`), в центре — маленький белый круговой спиннер (macOS-стиль, сглаживание, 60 fps).
- **Переход**: плавный кроссфейд 1.4 с в рабочий стол.
- **Рабочий стол**: обои + пустая верхняя панель в стиле macOS («матовое стекло»: box blur обоев + осветление) + счётчик FPS в правом верхнем углу.
- **FPS — реальный замер**: считается количество действительно отрисованных кадров (окно 0.5 с). Никаких фейковых данных в системе нет вообще: логи — настоящие сообщения ядра, CPU/RAM — из `/proc/cpuinfo` и `/proc/meminfo`.

## Быстрый старт (Windows + QEMU)

```cmd
"C:\Program Files\qemu\qemu-system-x86_64.exe" -m 2048 -cdrom "%USERPROFILE%\Downloads\AquaOS.iso" -vga std
```

С ускорением (если включён Windows Hypervisor Platform) добавьте `-accel whpx`. Готовая ISO лежит в [`iso/AquaOS.iso`](iso/AquaOS.iso). ISO гибридный — можно записать `dd` на USB-флешку.

Под Linux: `qemu-system-x86_64 -m 2048 -cdrom AquaOS.iso -enable-kvm -vga std`.

## Сборка

Нужен Debian/Ubuntu-подобный Linux. Без root:

```bash
./tools_setup.sh        # скачает .deb и распакует тулчейн в tools/ (без установки)
./build.sh              # соберёт ISO в build/AquaOS.iso
WALLPAPER=/путь/к/картинке.jpg ./build.sh   # со своей обоей
DEBUG=1 ./build.sh      # отладочный ISO: весь kmsg дублируется в COM1
```

С root можно поставить пакеты штатно: `apt install build-essential busybox-static grub-pc-bin grub-common xorriso cpio python3-pil linux-image-amd64`.

## Структура

```
├── src/splash.c        # графическая оболочка: kmsg-логи, спиннер, кроссфейд,
│                       # панель macOS (blur-«стекло»), FPS-счётчик
├── src/init            # PID 1: монтирование ФС, mdev, инфо CPU/RAM, сигнал bootdone
├── src/build.sh        # полный пайплайн сборки ISO
├── src/mkfont.py       # генерация шрифта 8x16 (DejaVu Sans Mono → font.h)
├── src/mkwallpaper.py  # обои jpg → raw RGB 1024x768 (cover-crop)
├── src/test_qemu.py    # автотест: QEMU headless + скриншоты по таймингам
├── tools_setup.sh      # бутстрап тулчейна без root
├── assets/wallpaper.jpg# обои по умолчанию
├── iso/AquaOS.iso      # последняя собранная ISO
└── screenshots/        # скриншоты из QEMU
```

Подробности — в [INSTRUCTION.md](INSTRUCTION.md): архитектура, настройка констант, скрытые возможности (recovery-консоль Ctrl-Alt-F2, отладка через COM1).

## Восстановление из бэкапа

Полная история git лежит в одном файле `aquaos.bundle` (рядом с ISO в деливераблах):

```cmd
git clone aquaos.bundle AquaOS
```
