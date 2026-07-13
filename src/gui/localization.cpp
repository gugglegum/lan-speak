#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "gui/localization.h"

#include <windows.h>

namespace lanspeak::gui {
namespace {

enum class UiLanguage {
    russian,
    english
};

UiLanguage detect_windows_ui_language() {
    const LANGID user_language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(user_language) == LANG_RUSSIAN) {
        return UiLanguage::russian;
    }
    const LANGID system_language = GetSystemDefaultUILanguage();
    return PRIMARYLANGID(system_language) == LANG_RUSSIAN ? UiLanguage::russian : UiLanguage::english;
}

UiLanguage current_ui_language(LanguageSetting setting) {
    switch (setting) {
    case LanguageSetting::russian:
        return UiLanguage::russian;
    case LanguageSetting::english:
        return UiLanguage::english;
    case LanguageSetting::automatic:
    default:
        return detect_windows_ui_language();
    }
}


} // namespace

const wchar_t* localized_text(TextId id, LanguageSetting setting) {
    const bool ru = current_ui_language(setting) == UiLanguage::russian;
    switch (id) {
    case TextId::file:
        return ru ? L"Файл" : L"File";
    case TextId::capture_device:
        return ru ? L"Устройство ввода" : L"Input device";
    case TextId::render_device:
        return ru ? L"Устройство вывода" : L"Output device";
    case TextId::mic_gain:
        return ru ? L"Усил. микрофона" : L"Mic gain";
    case TextId::settings:
        return ru ? L"Настройки" : L"Settings";
    case TextId::language:
        return ru ? L"Язык" : L"Language";
    case TextId::language_auto:
        return ru ? L"Авто" : L"Auto";
    case TextId::language_russian:
        return ru ? L"Русский" : L"Russian";
    case TextId::language_english:
        return ru ? L"Английский" : L"English";
    case TextId::start:
        return ru ? L"Старт" : L"Start";
    case TextId::stop:
        return ru ? L"Стоп" : L"Stop";
    case TextId::talk:
        return ru ? L"Говорить" : L"Talk";
    case TextId::talking:
        return ru ? L"Говорю..." : L"Talking...";
    case TextId::talk_mode:
        return ru ? L"Режим:" : L"Mode:";
    case TextId::talk_mode_toggle:
        return ru ? L"Переключение" : L"Toggle";
    case TextId::talk_mode_hold:
        return ru ? L"Пока нажимаю" : L"Hold to talk";
    case TextId::add_contact:
        return ru ? L"Добавить контакт" : L"Add contact";
    case TextId::edit:
        return ru ? L"Редактировать" : L"Edit";
    case TextId::delete_contact:
        return ru ? L"Удалить" : L"Delete";
    case TextId::default_capture_device:
        return ru ? L"Устройство ввода по умолчанию" : L"Default input device";
    case TextId::default_render_device:
        return ru ? L"Устройство вывода по умолчанию" : L"Default output device";
    case TextId::unnamed_contact:
        return ru ? L"<без имени>" : L"<unnamed>";
    case TextId::gain_short:
        return ru ? L"усил" : L"gain";
    case TextId::duck_short:
        return ru ? L"пригл" : L"duck";
    case TextId::off:
        return ru ? L"выкл" : L"off";
    case TextId::name:
        return ru ? L"Имя" : L"Name";
    case TextId::ip_host:
        return ru ? L"IP / хост" : L"IP / host";
    case TextId::port:
        return ru ? L"Порт" : L"Port";
    case TextId::contact_gain:
        return ru ? L"Усиление" : L"Contact gain";
    case TextId::self_ducking:
        return ru ? L"Глушить своё эхо" : L"Self ducking";
    case TextId::threshold:
        return ru ? L"Порог" : L"Threshold";
    case TextId::attack_ms:
        return ru ? L"Атака, мс" : L"Attack ms";
    case TextId::hold_ms:
        return ru ? L"Удерж., мс" : L"Hold ms";
    case TextId::release_ms:
        return ru ? L"Восст., мс" : L"Release ms";
    case TextId::save:
        return ru ? L"Сохранить" : L"Save";
    case TextId::add:
        return ru ? L"Добавить" : L"Add";
    case TextId::cancel:
        return ru ? L"Отмена" : L"Cancel";
    case TextId::edit_contact_title:
        return ru ? L"Редактировать контакт" : L"Edit contact";
    case TextId::add_contact_title:
        return ru ? L"Добавить контакт" : L"Add contact";
    case TextId::name_required:
        return ru ? L"Имя обязательно." : L"Name is required.";
    case TextId::host_required:
        return ru ? L"IP / хост обязателен." : L"IP / host is required.";
    case TextId::port_required:
        return ru ? L"Порт обязателен и должен быть в диапазоне 1..65535."
                  : L"Port is required and must be 1..65535.";
    case TextId::save_settings_failed:
        return L"Could not save settings: ";
    case TextId::device_refresh_failed:
        return L"Device refresh failed: ";
    case TextId::device_refresh_unusable:
        return L"Device refresh did not return a usable list.\r\n";
    case TextId::loaded_devices:
        return L"Loaded devices: ";
    case TextId::already_running:
        return L"LanSpeak is already running.\r\n";
    case TextId::listener_idle:
        return L"Listener is idle: add at least one contact.\r\n";
    case TextId::add_contact_before_start:
        return L"Add at least one contact before starting.\r\n";
    case TextId::core_missing:
        return L"Core executable was not found next to GUI: ";
    case TextId::create_pipe_failed:
        return L"CreatePipe failed.\r\n";
    case TextId::starting:
        return L"\r\nStarting LanSpeak ";
    case TextId::create_process_failed:
        return L"CreateProcess failed, GetLastError=";
    case TextId::restarting:
        return L"\r\nRestarting LanSpeak...\r\n";
    case TextId::stopping:
        return L"\r\nStopping LanSpeak...\r\n";
    case TextId::stopped:
        return L"\r\nLanSpeak stopped.\r\n";
    case TextId::no_contacts_left:
        return L"\r\nNo contacts left; stopping LanSpeak.\r\n";
    case TextId::already_in_mode:
        return L"LanSpeak is already in ";
    case TextId::listener_mode:
        return L"listener";
    case TextId::duplex_mode:
        return L"duplex";
    case TextId::global_hotkeys:
        return ru ? L"Глобальные хоткеи" : L"Global Hotkeys";
    case TextId::debug_console:
        return ru ? L"Отладочная консоль" : L"Debug console";
    case TextId::ptt_for_all:
        return ru ? L"PTT для всех" : L"PTT for All";
    case TextId::osd_me:
        return ru ? L"Я" : L"Me";
    case TextId::contact_hotkeys:
        return ru ? L"Контакты" : L"Contacts";
    case TextId::hotkey_not_set:
        return ru ? L"Не задан" : L"Not set";
    case TextId::hotkey_record:
        return ru ? L"Задать" : L"Set";
    case TextId::hotkey_recording:
        return ru ? L"Нажмите сочетание клавиш..." : L"Press a key combination...";
    case TextId::hotkey_clear:
        return ru ? L"Очистить" : L"Clear";
    case TextId::tray_show:
        return ru ? L"Показать окно" : L"Show window";
    case TextId::tray_exit:
        return ru ? L"Выход" : L"Exit";
    case TextId::include_in_global_ptt:
        return ru ? L"Включить в PTT для всех" : L"Include in PTT for All";
    case TextId::exclude_from_global_ptt:
        return ru ? L"Исключить из PTT для всех" : L"Exclude from PTT for All";
    case TextId::help:
        return ru ? L"Помощь" : L"Help";
    case TextId::about:
        return ru ? L"О программе" : L"About";
    case TextId::about_product_version:
        return ru ? L"LAN Speak версия 1.0.0" : L"LAN Speak version 1.0.0";
    case TextId::about_author:
        return ru ? L"Автор программы: Paul Melekhov" : L"Author: Paul Melekhov";
    case TextId::about_github:
        return L"GitHub:";
    case TextId::ok:
        return L"OK";
    default:
        return L"";
    }
}


} // namespace lanspeak::gui
