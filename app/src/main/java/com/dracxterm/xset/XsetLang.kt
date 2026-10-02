package com.dracxterm.xset

/**
 * xset UI localization (settings-language feature).
 *
 * Six languages, Indonesian is the default. The displayed language of the whole xset dashboard
 * follows the `ui.lang` store value. Switching is LIVE and IN-PLACE: [XsetController.render] syncs
 * [XsetI18n.current] from the store every frame and invalidates the row cache when it changed, so the
 * same overlay surface simply repaints with the new text — no new window, no flicker. Module titles,
 * row labels, options, actions, footer, status and chrome all resolve through [XsetI18n.t] at
 * render/build time, so nothing is captured in the old language.
 *
 * Technical literals (paths, versions, model names, colour hex, "JetBrains Mono", "ON/OFF", units,
 * "canonical JSON", "vsync", …) are intentionally NOT translated: they are stable across languages.
 */
enum class XsetLang(val id: String, val display: String) {
    INDONESIA("indonesia", "Indonesia"),
    ENGLISH("english", "English"),
    CHINA("china", "中文"),
    VIETNAM("vietnam", "Tiếng Việt"),
    RUSIA("rusia", "Русский"),
    INDIA("india", "हिन्दी");

    companion object {
        fun from(id: String?): XsetLang = values().firstOrNull { it.id.equals(id, true) } ?: INDONESIA
        /** Options for the Language picker, in registration order (value = canonical id). */
        fun options(): List<Opt> = values().map { Opt(it.display, it.id) }
    }
}

object XsetI18n {
    /** Current UI language. Kept in sync with the store's `ui.lang` by the controller each render. */
    @Volatile
    var current: XsetLang = XsetLang.INDONESIA

    fun sync(store: XsetStore) { current = XsetLang.from(store.get("ui.lang")) }

    /** Translate [key] into [current]; falls back to Indonesian then to the raw key. */
    fun t(key: String): String {
        val row = TABLE[key] ?: return key
        return row.getOrNull(current.ordinal)?.takeIf { it.isNotEmpty() }
            ?: row.getOrNull(0)?.takeIf { it.isNotEmpty() }
            ?: key
    }

    // Column order MUST match the enum ordinal: [ID, EN, ZH, VI, RU, HI].
    private val TABLE: Map<String, Array<String>> = mapOf(
        // ---- module titles ----
        "mod.language"    to arrayOf("Bahasa", "Language", "语言", "Ngôn ngữ", "Язык", "भाषा"),
        "mod.appearance"  to arrayOf("Tampilan", "Appearance", "外观", "Giao diện", "Оформление", "रूप"),
        "mod.theme"       to arrayOf("Tema", "Theme", "主题", "Chủ đề", "Тема", "थीम"),
        "mod.font"        to arrayOf("Font", "Font", "字体", "Phông chữ", "Шрифт", "फ़ॉन्ट"),
        "mod.cursor"      to arrayOf("Kursor", "Cursor", "光标", "Con trỏ", "Курсор", "कर्सर"),
        "mod.background"  to arrayOf("Latar", "Background", "背景", "Nền", "Фон", "पृष्ठभूमि"),
        "mod.performance" to arrayOf("Kinerja", "Performance", "性能", "Hiệu năng", "Производительность", "प्रदर्शन"),
        "mod.storage"     to arrayOf("Akses Penyimpanan", "Storage Access", "存储访问", "Truy cập bộ nhớ", "Доступ к хранилищу", "स्टोरेज एक्सेस"),
        "mod.claw"        to arrayOf("AI (claw)", "AI (claw)", "AI (claw)", "AI (claw)", "ИИ (claw)", "AI (claw)"),
        "mod.diagnostics" to arrayOf("Diagnostik", "Diagnostics", "诊断", "Chẩn đoán", "Диагностика", "निदान"),
        "mod.backup"      to arrayOf("Cadangan", "Backup", "备份", "Sao lưu", "Резервная копия", "बैकअप"),
        "mod.about"       to arrayOf("Tentang", "About", "关于", "Giới thiệu", "О программе", "परिचय"),

        // ---- setting labels ----
        "lbl.ui_lang"       to arrayOf("Bahasa xset", "xset language", "xset 语言", "Ngôn ngữ xset", "Язык xset", "xset भाषा"),
        "lbl.preset"        to arrayOf("Praset", "Preset", "预设", "Cài sẵn", "Пресет", "प्रीसेट"),
        "lbl.theme"         to arrayOf("Tema", "Theme", "主题", "Chủ đề", "Тема", "थीम"),
        "lbl.foreground"    to arrayOf("Warna depan", "Foreground", "前景色", "Màu chữ", "Текст", "अग्रभूमि"),
        "lbl.background"    to arrayOf("Latar", "Background", "背景色", "Màu nền", "Фон", "पृष्ठभूमि"),
        "lbl.cursor_color"  to arrayOf("Warna kursor", "Cursor Color", "光标颜色", "Màu con trỏ", "Цвет курсора", "कर्सर रंग"),
        "lbl.font_size"     to arrayOf("Ukuran font", "Font Size", "字号", "Cỡ chữ", "Размер шрифта", "फ़ॉन्ट आकार"),
        "lbl.size_dp"       to arrayOf("Ukuran (dp)", "Size (dp)", "大小 (dp)", "Cỡ (dp)", "Размер (dp)", "आकार (dp)"),
        "lbl.family"        to arrayOf("Keluarga", "Family", "字族", "Họ phông", "Семейство", "परिवार"),
        "lbl.line_spacing"  to arrayOf("Spasi baris %", "Line Spacing %", "行距 %", "Giãn dòng %", "Межстрочный %", "पंक्ति अंतराल %"),
        "lbl.letter_spacing" to arrayOf("Spasi huruf", "Letter Spacing", "字间距", "Giãn chữ", "Межбуквенный", "अक्षर अंतराल"),
        "lbl.bold_bright"   to arrayOf("Tebal = Terang", "Bold = Bright", "粗体=高亮", "Đậm = Sáng", "Жирный = Яркий", "बोल्ड = चमकीला"),
        "lbl.padding"       to arrayOf("Padding %", "Padding %", "内边距 %", "Đệm %", "Отступ %", "पैडिंग %"),
        "lbl.cursor"        to arrayOf("Kursor", "Cursor", "光标", "Con trỏ", "Курсор", "कर्सर"),
        "lbl.cursor_blink"  to arrayOf("Kedip kursor", "Cursor Blink", "光标闪烁", "Nháy con trỏ", "Мигание курсора", "कर्सर ब्लिंक"),
        "lbl.style"         to arrayOf("Gaya", "Style", "样式", "Kiểu", "Стиль", "शैली"),
        "lbl.blink"         to arrayOf("Kedip", "Blink", "闪烁", "Nháy", "Мигание", "ब्लिंक"),
        "lbl.color"         to arrayOf("Warna", "Color", "颜色", "Màu", "Цвет", "रंग"),
        "lbl.contrast_fg"   to arrayOf("Kontras (depan)", "Contrast (fg)", "对比 (前景)", "Tương phản (chữ)", "Контраст (текст)", "कंट्रास्ट (अग्र)"),
        "lbl.scrollback"    to arrayOf("Baris scrollback", "Scrollback lines", "回滚行数", "Dòng cuộn lại", "Строки прокрутки", "स्क्रॉलबैक पंक्तियाँ"),
        "lbl.storage_access" to arrayOf("Akses penyimpanan", "Storage Access", "存储访问", "Truy cập bộ nhớ", "Доступ к хранилищу", "स्टोरेज एक्सेस"),
        "lbl.browsing_web"  to arrayOf("Browsing web", "Web browsing", "网页浏览", "Duyệt web", "Веб-доступ", "वेब ब्राउज़िंग"),
        "lbl.fs_access"     to arrayOf("Kelola berkas & terminal", "Files & terminal", "文件与终端", "Tệp & terminal", "Файлы и терминал", "फ़ाइलें व टर्मिनल"),
        "lbl.private_network" to arrayOf("Izinkan jaringan lokal", "Allow local network", "允许本地网络", "Cho phép mạng nội bộ", "Разрешить локальную сеть", "लोकल नेटवर्क अनुमति"),
        "lbl.use_skills"    to arrayOf("Pakai skill", "Use skills", "使用技能", "Dùng kỹ năng", "Использовать навыки", "स्किल्स उपयोग"),
        "lbl.chat_model"    to arrayOf("Model claw chat", "claw chat model", "claw chat 模型", "Mô hình claw chat", "Модель claw chat", "claw chat मॉडल"),
        "lbl.humanizer"     to arrayOf("Humanizer (agen otonom)", "Humanizer (autonomous)", "Humanizer (自主代理)", "Humanizer (tự động)", "Humanizer (автономный)", "Humanizer (स्वायत्त)"),
        "lbl.humanizer_model" to arrayOf("Model humanizer", "Humanizer model", "Humanizer 模型", "Mô hình humanizer", "Модель humanizer", "Humanizer मॉडल"),
        "lbl.humanizer_agents" to arrayOf("Agen humanizer maks", "Max humanizer agents", "Humanizer 最大代理数", "Số agent humanizer tối đa", "Макс. агентов humanizer", "अधिकतम humanizer एजेंट"),
        "lbl.auto_update"   to arrayOf("Auto-update daftar model", "Auto-update model list", "自动更新模型列表", "Tự cập nhật danh sách mô hình", "Автообновление списка моделей", "मॉडल सूची ऑटो-अपडेट"),
        "lbl.update_hours"  to arrayOf("Update tiap (jam)", "Update every (hours)", "更新间隔 (小时)", "Cập nhật mỗi (giờ)", "Обновлять каждые (ч)", "अपडेट हर (घंटे)"),
        "lbl.proxy_token"   to arrayOf("Wajib token untuk serve", "Require token for serve", "serve 需令牌", "Bắt buộc token cho serve", "Токен для serve", "serve हेतु टोकन"),
        "lbl.save_conversations" to arrayOf("Simpan isi percakapan", "Save conversations", "保存对话", "Lưu hội thoại", "Сохранять переписку", "वार्तालाप सहेजें"),
        "lbl.activity_log"  to arrayOf("Catat aktivitas ke file", "Log activity to file", "记录活动到文件", "Ghi hoạt động ra tệp", "Журнал активности в файл", "गतिविधि लॉग फ़ाइल"),
        // info labels
        "lbl.active"        to arrayOf("Aktif", "Active", "当前", "Đang dùng", "Активно", "सक्रिय"),
        "lbl.renderer"      to arrayOf("Perender", "Renderer", "渲染器", "Trình kết xuất", "Рендерер", "रेंडरर"),
        "lbl.shapes"        to arrayOf("Bentuk", "Shapes", "形状", "Hình dạng", "Формы", "आकृतियाँ"),
        "lbl.note"          to arrayOf("Catatan", "Note", "备注", "Ghi chú", "Примечание", "टिप्पणी"),
        "lbl.permission"    to arrayOf("Izin", "Permission", "权限", "Quyền", "Разрешение", "अनुमति"),
        "lbl.mount_point"   to arrayOf("Titik mount", "Mount point", "挂载点", "Điểm gắn", "Точка монтирования", "माउंट बिंदु"),
        "lbl.contents"      to arrayOf("Isi", "Contents", "内容", "Nội dung", "Содержимое", "सामग्री"),
        "lbl.applies"       to arrayOf("Berlaku", "Applies", "生效", "Áp dụng", "Применяется", "लागू"),
        "lbl.if_denied"     to arrayOf("Jika ditolak", "If denied", "若被拒绝", "Nếu bị từ chối", "Если отказано", "यदि अस्वीकृत"),
        "lbl.rendering"     to arrayOf("Rendering", "Rendering", "渲染", "Kết xuất", "Отрисовка", "रेंडरिंग"),
        "lbl.frame_pacing"  to arrayOf("Frame pacing", "Frame pacing", "帧节奏", "Nhịp khung hình", "Темп кадров", "फ़्रेम पेसिंग"),
        "lbl.buffer"        to arrayOf("Buffer", "Buffer", "缓冲", "Bộ đệm", "Буфер", "बफ़र"),
        "lbl.autosave"      to arrayOf("Simpan otomatis", "Auto-save", "自动保存", "Tự lưu", "Автосохранение", "ऑटो-सेव"),
        "lbl.keys"          to arrayOf("Kunci", "Keys", "键数", "Số khóa", "Ключи", "कुंजियाँ"),
        "lbl.format"        to arrayOf("Format", "Format", "格式", "Định dạng", "Формат", "प्रारूप"),
        "lbl.status"        to arrayOf("Status", "Status", "状态", "Trạng thái", "Статус", "स्थिति"),
        "lbl.browsing_scrapling" to arrayOf("Browsing (Scrapling)", "Browsing (Scrapling)", "浏览 (Scrapling)", "Duyệt (Scrapling)", "Браузинг (Scrapling)", "ब्राउज़िंग (Scrapling)"),
        "lbl.perm_stored"   to arrayOf("Izin disimpan di", "Permissions stored in", "权限存储于", "Lưu quyền tại", "Разрешения хранятся в", "अनुमति संग्रह स्थान"),
        "lbl.in_effect"     to arrayOf("Berlaku", "In effect", "生效时机", "Có hiệu lực", "Вступает в силу", "प्रभावी"),
        "lbl.chat"          to arrayOf("Ngobrol", "Chat", "聊天", "Trò chuyện", "Чат", "चैट"),
        "lbl.auto_mode"     to arrayOf("Mode otonom", "Autonomous mode", "自主模式", "Chế độ tự động", "Автономный режим", "स्वायत्त मोड"),
        "lbl.sensitive"     to arrayOf("Data sensitif", "Sensitive data", "敏感数据", "Dữ liệu nhạy cảm", "Чувствительные данные", "संवेदनशील डेटा"),
        "lbl.library"       to arrayOf("Pustaka", "Library", "库", "Thư viện", "Библиотека", "लाइब्रेरी"),
        "lbl.abi_version"   to arrayOf("Versi ABI", "ABI version", "ABI 版本", "Phiên bản ABI", "Версия ABI", "ABI संस्करण"),
        "lbl.build_arch"    to arrayOf("Arsitektur build", "Build arch", "构建架构", "Kiến trúc build", "Архитектура сборки", "बिल्ड आर्क"),
        "lbl.detected_profile" to arrayOf("Profil terdeteksi", "Detected profile", "检测到的配置", "Hồ sơ phát hiện", "Обнаруженный профиль", "पहचाना प्रोफ़ाइल"),
        "lbl.exec_policy"   to arrayOf("Kebijakan exec-storage", "Exec-storage policy", "Exec-storage 策略", "Chính sách exec-storage", "Политика exec-storage", "Exec-storage नीति"),
        "lbl.androidapp"    to arrayOf("Dukungan android-app", "android-app support", "android-app 支持", "Hỗ trợ android-app", "Поддержка android-app", "android-app समर्थन"),
        "lbl.guest_backend" to arrayOf("Backend guest", "Guest backend", "Guest 后端", "Backend guest", "Бэкенд гостя", "गेस्ट बैकएंड"),
        "lbl.contact_info"  to arrayOf("Informasi kontak", "Contact information", "联系方式", "Thông tin liên hệ", "Контактная информация", "संपर्क जानकारी"),
        "lbl.contact_for"   to arrayOf("Kontak untuk", "Contact for", "联系用于", "Liên hệ để", "Связаться для", "संपर्क हेतु"),
        "lbl.also"          to arrayOf("Juga", "Also", "也", "Cũng", "Также", "साथ ही"),

        // ---- actions ----
        "act.reset_theme"   to arrayOf("Reset Tema", "Reset Theme", "重置主题", "Đặt lại chủ đề", "Сбросить тему", "थीम रीसेट"),
        "act.reset_font"    to arrayOf("Reset Font", "Reset Font", "重置字体", "Đặt lại phông", "Сбросить шрифт", "फ़ॉन्ट रीसेट"),
        "act.reset_cursor"  to arrayOf("Reset Kursor", "Reset Cursor", "重置光标", "Đặt lại con trỏ", "Сбросить курсор", "कर्सर रीसेट"),
        "act.reset_background" to arrayOf("Reset Latar", "Reset Background", "重置背景", "Đặt lại nền", "Сбросить фон", "पृष्ठभूमि रीसेट"),
        "act.save_now"      to arrayOf("Simpan Sekarang", "Save Now", "立即保存", "Lưu ngay", "Сохранить", "अभी सहेजें"),
        "act.export"        to arrayOf("Ekspor ke Berkas", "Export to File", "导出到文件", "Xuất ra tệp", "Экспорт в файл", "फ़ाइल में निर्यात"),
        "act.import"        to arrayOf("Impor dari Berkas", "Import from File", "从文件导入", "Nhập từ tệp", "Импорт из файла", "फ़ाइल से आयात"),
        "act.reset_all"     to arrayOf("Reset SEMUA ke bawaan", "Reset ALL to defaults", "全部重置为默认", "Đặt lại TẤT CẢ về mặc định", "Сбросить ВСЁ", "सभी डिफ़ॉल्ट रीसेट"),
        "act.grant_rebuild" to arrayOf("Beri / Bangun ulang", "Grant / Rebuild", "授予 / 重建", "Cấp / Dựng lại", "Выдать / Пересобрать", "ग्रांट / पुनर्निर्माण"),
        "act.apply_now"     to arrayOf("Terapkan sekarang", "Apply now", "立即应用", "Áp dụng ngay", "Применить сейчас", "अभी लागू करें"),
        "act.run_probes"    to arrayOf("Jalankan probe aktif", "Run active probes", "运行主动探测", "Chạy kiểm tra chủ động", "Запустить активные проверки", "सक्रिय जाँच चलाएँ"),
        "act.close"         to arrayOf("Tutup", "Close", "关闭", "Đóng", "Закрыть", "बंद करें"),

        // ---- options ----
        "opt.off"           to arrayOf("Mati", "Off", "关闭", "Tắt", "Выкл", "बंद"),
        "opt.ask"           to arrayOf("Tanya dulu", "Ask first", "先询问", "Hỏi trước", "Спрашивать", "पहले पूछें"),
        "opt.allow"         to arrayOf("Diizinkan", "Allowed", "允许", "Cho phép", "Разрешено", "अनुमत"),
        "opt.block"         to arrayOf("Blok", "Block", "块", "Khối", "Блок", "ब्लॉक"),
        "opt.bar"           to arrayOf("Bar", "Bar", "竖线", "Thanh", "Полоса", "बार"),
        "opt.underline"     to arrayOf("Garis bawah", "Underline", "下划线", "Gạch chân", "Подчёркивание", "अंडरलाइन"),
        "opt.hollow"        to arrayOf("Kosong", "Hollow", "空心", "Rỗng", "Контур", "खोखला"),
        "opt.auto"          to arrayOf("Otomatis", "Auto", "自动", "Tự động", "Авто", "स्वतः"),

        // ---- chrome / footer / status ----
        "chrome.subtitle"   to arrayOf("Kerangka Konfigurasi Terminal", "Terminal Configuration Framework", "终端配置框架", "Khung cấu hình Terminal", "Платформа настройки терминала", "टर्मिनल कॉन्फ़िगरेशन फ़्रेमवर्क"),
        "chrome.customization" to arrayOf("KUSTOMISASI", "CUSTOMIZATION", "自定义", "TÙY CHỈNH", "НАСТРОЙКА", "कस्टमाइज़ेशन"),
        "chrome.options"    to arrayOf("opsi", "options", "项", "tùy chọn", "опций", "विकल्प"),
        "chrome.preview"    to arrayOf("PRATINJAU", "LIVE PREVIEW", "实时预览", "XEM TRƯỚC", "ПРЕДПРОСМОТР", "लाइव प्रीव्यू"),
        "foot.move"         to arrayOf("Pindah", "Move", "移动", "Di chuyển", "Навигация", "चाल"),
        "foot.adjust"       to arrayOf("Ubah", "Adjust", "调整", "Chỉnh", "Изменить", "समायोजित"),
        "foot.apply"        to arrayOf("Terapkan", "Apply", "应用", "Áp dụng", "Применить", "लागू"),
        "foot.back"         to arrayOf("Kembali", "Back", "返回", "Quay lại", "Назад", "वापस"),
        "foot.search"       to arrayOf("Cari", "Search", "搜索", "Tìm", "Поиск", "खोज"),
        "foot.save"         to arrayOf("Simpan", "Save", "保存", "Lưu", "Сохр.", "सहेजें"),
        "foot.exit"         to arrayOf("Keluar", "Exit", "退出", "Thoát", "Выход", "बाहर"),
        "st.saved"          to arrayOf("Konfigurasi tersimpan ✓", "Configuration saved ✓", "配置已保存 ✓", "Đã lưu cấu hình ✓", "Конфигурация сохранена ✓", "कॉन्फ़िग सहेजा गया ✓"),
        "st.search_cancelled" to arrayOf("pencarian dibatalkan", "search cancelled", "搜索已取消", "đã hủy tìm", "поиск отменён", "खोज रद्द"),
        "st.no_match"       to arrayOf("tidak ada kecocokan", "no match", "无匹配", "không khớp", "нет совпадений", "कोई मेल नहीं"),
        "st.found"          to arrayOf("ketemu", "found", "找到", "đã tìm thấy", "найдено", "मिला"),
        "st.found_module"   to arrayOf("ketemu modul", "found module", "找到模块", "tìm thấy mô-đun", "найден модуль", "मॉड्यूल मिला"),
        "st.needs_larger"   to arrayOf("xset butuh layar lebih besar", "xset needs a larger view", "xset 需要更大的视图", "xset cần màn hình lớn hơn", "xset нужен экран побольше", "xset को बड़ा व्यू चाहिए"),

        // ---- info values (prose) ----
        "val.claw_installed" to arrayOf("terpasang di /opt/claw", "installed at /opt/claw", "已安装于 /opt/claw", "đã cài tại /opt/claw", "установлено в /opt/claw", "/opt/claw में स्थापित"),
        "val.claw_need_rootfs" to arrayOf("perlu Linux rootfs", "needs Linux rootfs", "需要 Linux rootfs", "cần Linux rootfs", "нужен Linux rootfs", "Linux rootfs चाहिए"),
        "val.ready"         to arrayOf("siap", "ready", "就绪", "sẵn sàng", "готово", "तैयार"),
        "val.browsing_auto" to arrayOf("disiapkan otomatis saat Linux terpasang", "auto-provisioned when Linux is installed", "安装 Linux 时自动配置", "tự thiết lập khi cài Linux", "настраивается при установке Linux", "Linux स्थापित होने पर स्वतः तैयार"),
        "val.when_next"     to arrayOf("saat claw dijalankan berikutnya", "on claw's next run", "claw 下次运行时", "ở lần chạy claw kế tiếp", "при следующем запуске claw", "claw के अगले रन पर"),
        "val.safe_default"  to arrayOf("humanizer & jaringan lokal default mati", "humanizer & local network off by default", "humanizer 与本地网络默认关闭", "humanizer & mạng nội bộ mặc định tắt", "humanizer и локальная сеть выкл. по умолчанию", "humanizer व लोकल नेटवर्क डिफ़ॉल्ट बंद"),
        "val.storage_mount" to arrayOf("~/sdcard (+ ~/sdcard-1 untuk volume lepasan)", "~/sdcard (+ ~/sdcard-1 for a removable volume)", "~/sdcard (+ ~/sdcard-1 为可移动卷)", "~/sdcard (+ ~/sdcard-1 cho ổ rời)", "~/sdcard (+ ~/sdcard-1 для съёмного тома)", "~/sdcard (+ हटाने योग्य हेतु ~/sdcard-1)"),
        "val.storage_contents" to arrayOf("akar volume asli (DCIM · Download · Android · …)", "real volume root (DCIM · Download · Android · …)", "真实卷根目录 (DCIM · Download · Android · …)", "gốc ổ thực (DCIM · Download · Android · …)", "корень тома (DCIM · Download · Android · …)", "वास्तविक वॉल्यूम रूट (DCIM · Download · Android · …)"),
        "val.storage_applies" to arrayOf("langsung di workspace aktif (tanpa restart)", "immediately on the active workspace (no restart)", "立即作用于当前工作区 (无需重启)", "ngay trên workspace đang dùng (không khởi động lại)", "сразу в активном рабочем пространстве (без перезапуска)", "सक्रिय वर्कस्पेस पर तुरंत (रीस्टार्ट नहीं)"),
        "val.storage_safe"  to arrayOf("terminal tetap jalan (tanpa crash)", "terminal keeps running (no crash)", "终端继续运行 (不崩溃)", "terminal vẫn chạy (không crash)", "терминал продолжает работать (без сбоя)", "टर्मिनल चालू रहता है (कोई क्रैश नहीं)"),
        "val.autosave_on"   to arrayOf("aktif (tiap perubahan tersimpan)", "on (every change persists)", "开启 (每次更改即保存)", "bật (mọi thay đổi được lưu)", "вкл (каждое изменение сохраняется)", "चालू (हर बदलाव सहेजा)"),
        "val.fmt_json"      to arrayOf("JSON kanonik (portabel)", "canonical JSON (portable)", "规范 JSON (可移植)", "JSON chuẩn (khả chuyển)", "канонический JSON (переносимый)", "कैनॉनिकल JSON (पोर्टेबल)"),
        "val.contact_for"   to arrayOf("masukan · laporan bug · fitur", "feedback · bug reports · features", "反馈 · 错误报告 · 功能", "góp ý · báo lỗi · tính năng", "отзывы · баги · функции", "फ़ीडबैक · बग · फ़ीचर"),
        "val.also"          to arrayOf("perbaikan · masalah kompatibilitas", "fixes · compatibility issues", "修复 · 兼容性问题", "sửa lỗi · vấn đề tương thích", "исправления · совместимость", "फ़िक्स · संगतता मुद्दे"),
    )
}
