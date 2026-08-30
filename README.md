# tb-tray

GTK3 system tray applet + Thunderbird MailExtension: непрочитанная почта
в трее, уведомления о новых письмах, запуск Thunderbird в скрытом режиме.

## Состав

- `tray/tb-tray.c` — апплет трея (GTK3):
  - локальный HTTP-сервер `127.0.0.1:8765` принимает репорты от плагина
    (`{"unread": N, "last": {"from": "...", "subject": "..."}}`)
  - иконка с счётчиком непрочитанных, tooltip «Thunderbird: N unread»
  - уведомление при увеличении непрочитанных
  - клик по иконке — запустить/показать Thunderbird
  - при старте апплета, если TB не запущен — запускает его **скрытно**
    (окно withdraw через X11)
- `extension/` — MailExtension (`tb-tray@kosmik2001`):
  - считает непрочитанные по всем аккаунтам (`messages.query`)
  - репортит в апплет при `onNewMailReceived` и раз в минуту

## Сборка

```
meson setup build -Dgtk3=true
ninja -C build
```

## Установка

- ebuild: `x11-misc/tb-tray` (kosmik2001-overlay)
  - `USE="extension"` (вкл. по умолчанию) — ставит MailExtension в профили
    Thunderbird (`~/.thunderbird/*/extensions/tb-tray@kosmik2001/`)
  - `USE="asus-led"` (выкл. по умолчанию) — HUD: апплет сообщает о новой почте
    asus-oled демону (mail-LED на ноутбуке), endpoint —
    `ASUS_LED_URL_DEFAULT=http://192.168.137.115:8077/mail`, переопределение —
    переменная `TB_LED_URL`
- ручная: `meson install -C build`, расширение — `scripts/make-xpi.sh` →
  TB → Add-ons → Install Add-on From File

## Автозапуск

Скопировать `data/tb-tray.desktop` в `~/.config/autostart/`.
