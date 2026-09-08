# Eye Tracking Foveation — статус работы (handoff для нового контекста)

_Обновлено: 2026-09-08. Ветка `feature/dlssnr-vr`, HEAD `04c5254f` + cleanup-коммиты._
_Диагностика движения вынесена в `docs/EyeTrackingFoveation_Diagnostics.md` (там же
чек-лист её удаления). Ранний гайд `docs/EyeTrackingFoveation_Implementation_Guide.md`
и `docs/EyeTrackingFoveation_QUICKREF.md` — исторические: их настройки
(`eyeTrackingGazeThresholdPixels`, `eyeTrackingGazeSmoothingAlpha`) и example-интеграция
удалены из кода._

---

## 1. Что это за задача

Eye-tracking-driven foveated rendering для Skyrim VR с DLSS:
Высококачественный DLSS-субрект (~1286x1025 из 4184x3256 на глаз) следует за взглядом
(трекер Pimax Dream, драйвер sboys3, OpenVR IVRSystem_026 через GetGenericInterface).
Остальная область рендерится дёшево. Neural Rendering (NGX Feature 18) работает
поверх DLSS внутри того же субректа.

## 2. Коммиты (по порядку)

| Коммит | Что |
|---|---|
| `89b54dba` | Интеграция LDR neural rendering |
| `635033c2` | NGX Feature 18 renderer (DLSS NR) |
| `96fcc697` | Screenshot финального VR-композита |
| `3d6748f1` | Батчинг VR-оценки NR и fence-ожиданий |
| `091bfb4d` | Стабилизация flat-пререквизитов |
| `05e037ca` | Flat pass до UI |
| `b013ed45` | Eye tracking базис: per-eye NDC gaze, subrect-офсеты, диагностика `[EYETRACK]`/`[FOVEATED-DIAG]` |
| `5cf7f7dd` | Pinhole-репроекция: gaze-офсет субректа (NDC) вшивается в проекционные матрицы для Streamline (curr = текущий офсет, prev = `Streamline::prevGazeOffsetNDC[2]`) → DLSS репроецирует историю при движении субректа, reset не нужен |
| `f4eb1425` | Deadzone + dwell, NR fade интенсивности, re-crop NR гайдов, `forceReset` в `Renderer::ApplyStereo` |
| `d9310a9e` | Saccade-байпас (40 px/кадр), glide 0.2, бинокулярный лок зон, NR fade 0.4/0.2 |
| `df247b47` | NR settle fade 0.35 (восстановление за ~7 кадров) |
| `9d5e5960` | Глайд-латч (глайд не останавливается по re-arm deadzone, доезжает до цели) |
| `7d65ea43` | `[GLIDE-DIAG]` — покадровая диагностика движения (см. `EyeTrackingFoveation_Diagnostics.md`) |
| `04c5254f` | **Ключевой фикс двойной перерисовки**: цель субректа = сырой gaze, не EMA-точка |

## 3. Архитектура стабилизации (текущая)

Проблема: человеческий взгляд НИКОГДА не неподвижен (fixation drift, микросаккады) +
дрейф трекера. Каждое движение субректа ломает DLSS/NR temporal history (мерцание).

Слои решения (в `FoveatedRender::UpdateEyeTrackingFoveation`, `FoveatedRender.cpp`):

1. **Deadzone** (`eyeTrackingFovDeadzonePx`, 50–800, дефолт 300): регион прыгает
   только при дрейфе gaze НА эту дистанцию ОТ ЗАКРЕПЛЁННОЙ точки.
2. **Dwell** (`kGazeDwellFrames = 8`): дрейф должен превышать deadzone 8 кадров
   ПОДРЯД (`gazeOverThresholdFrames[2]`), чтобы транзиентные скачки не двигали регион.
3. **Saccade-байпас** (`kSaccadeSpeedPx = 40`): рост дрейфа ≥ 40 px/кадр = осознанный
   перевод взгляда → движение стартует немедленно, без dwell. Убирает ~89 мс латентности.
4. **Бинокулярный лок**: решение двигаться общее — срабатывание любого глаза запускает
   движение ОБОИХ зон в одном кадре (каждая к своей цели). Per-eye гейты под
   конвергенцией срабатывают с разницей в 1–2 кадра → бинокулярное мерцание.
5. **Глайд-латч** (`gazeGlideActive`): после срабатывания гейта движение обеих зон
   продолжается каждый кадр, пока обе не доедут до цели (< 16 px), независимо от
   re-arm deadzone. Нужен для суб-1.0 glide.
6. **Raw-gaze цель** (`04c5254f`, главный фикс): цель субректа — СЫРОЙ
   bias-корректированный gaze, а НЕ EMA-точка. EMA-хвост (~10 кадров сходимости
   после саккады) заставлял регион, прыгнувший к сглаженной точке, отставать на
   сотни px → второй dwell-трип → «двойная перерисовка» (см.
   `EyeTrackingFoveation_Diagnostics.md`). EMA осталась только для debug-оверлея
   (`smoothedGazeLeft/Right`).
7. **Pinhole-репроекция DLSS** (`Streamline.cpp`): gaze-офсет вшивается в проекционные
   матрицы (curr = текущий офсет, prev = прошлый кадр) → DLSS переживает прыжки
   любого размера без потери истории.
8. **NR fade** (`NeuralRendering/Integration.cpp`): NR остаётся включённым каждый
   кадр; Intensity easing к 0 при движении (`kMoveFadePerFrame = 0.4`) и обратно
   (`kSettleFadePerFrame = 0.35`, ~7 кадров до полной силы). NR-гайды (depth/mvec)
   пере-вырезаются по текущим per-eye UV-боксам каждый кадр (re-crop вместо скипа).

### Итоговое поведение

- Перевод взгляда → **один телепорт** субректа в точку gaze (Glide Speed = 1.0),
  обе зоны синхронно, DLSS-история не теряется, NR восстанавливается за ~7 кадров.
- Фиксация → регион идеально неподвижен (deadzone/dwell).
- Пользователь проверил: «очень неплохо». Суб-1.0 glide «сыплет» картинку по ходу
  движения (DLSS-история пере-репроецируется на каждый шаг) — предпочтение
  пользователя: прыжок, а не глайд.

### Известный остаточный риск

Сырой gaze не сглажен → позиция приземления региона несёт шум трекера. Если
приземление станет «нервным» (позиция прыжка гуляет между одинаковыми взглядами) —
вернуть лёгкое сглаживание цели (EMA с alpha 0.4–0.5: сходимость за 2–3 кадра,
сохраняет одиночный прыжок).

## 4. Ключевые технические факты (не потерять)

- `sl::float4x4 = float4 row[4]`, аксессоров `_14/_24` НЕТ → `m[0].w`, `m[1].w`.
- DLSS пишет в `kMAIN`, NR работает с `kTOTAL`; eyeWidth=4184, размеры совпадают.
- «Два квадрата» в HMD = бинокулярное двоение → per-eye офсеты из СВОЕГО gaze
  каждого глаза (left.x ≠ right.x при конвергенции — норма).
- Gaze-хистерезис обязателен: каждое движение субректа ломает DLSS/NR temporal
  history. Pinhole-репроекция решает это для DLSS; для NR — fade интенсивности.
- «Двойная перерисовка» ≠ выключение DLSS: это две раздельные перецентровки региона
  из-за EMA-хвоста цели. Диагностируется по паттерну `S→D`-пар в `[GLIDE-DIAG]`.
- Миграция настроек: `eyeTrackingFovMoveThresholdPx` → `eyeTrackingFovDeadzonePx`
  (NLOHMANN обновлён); старые ini-значения молча отбрасываются.
- **Cleanup 2026-09-08:** удалён `src/Features/Upscaling/EyeTrackingFoveationImpl.h`
  (мёртвый example-заголовок: нигде не включался, ссылался на несуществующие
  настройки `gazeConfidenceThreshold`/`eyeTrackingGazeThresholdPixels`/
  `eyeTrackingGazeSmoothingAlpha`). Из `EyeTrackingFoveation.h` удалена
  неиспользуемая `HasSignificantGazeMovement`. Живой API заголовка:
  `EyeTrackingData`, `SmoothGazePoint`, `ClampGazeToValidRegion`, `GazeToSubrectOffset`.
- **Вылет-урок:** snap-итерация (<48 px snap + settle fade) давала вылет через
  несколько секунд после старта игры; откат вылет убрал. Повторять snap только с
  защитой ранних кадров (не снапить первые секунды после старта) и по частям.
- Сборка: пользователь собирает в Visual Studio (2026), НЕ из VS Code. В сборке
  VS 18 (toolset 14.51) MSB8074 warning про *.module.json безвреден.
  Warnings-as-errors: C4100 = ошибка. `get_errors` в VS Code не доверять для .cpp.
- Игровой лог: `C:\Users\ausho\Documents\My Games\Skyrim VR\SKSE\CommunityShaders.log`
  (читать самому — разрешено). Копия в `src/CommunityShaders.log` бывает устаревшей.

## 5. Тюнинг-константы (текущие значения)

| Константа | Значение | Где |
|---|---|---|
| `eyeTrackingFovDeadzonePx` | 300 (дефолт, слайдер 50–800) | Settings/UI |
| `eyeTrackingFovGlideFactor` | 1.0 у пользователя (дефолт 0.2, слайдер 0.05–1.0) | Settings/UI |
| `kGazeDwellFrames` | 8 | FoveatedRender.cpp |
| `kSaccadeSpeedPx` | 40 px/кадр | FoveatedRender.cpp |
| `kGazeSmoothingAlpha` | 0.2 (только debug-оверлей) | FoveatedRender.cpp |
| `kMoveFadePerFrame` (NR) | 0.4 | Integration.cpp |
| `kSettleFadePerFrame` (NR) | 0.35 | Integration.cpp |
| Глайд-латч порог прибытия | 16 px | FoveatedRender.cpp |
| Max subrect offset | 0.3 UV | FoveatedRender.cpp |

## 6. Что дальше (кандидаты)

- [ ] Продолжить тюнинг (пользователь продолжает игру с текущим билдом).
- [ ] По фидбеку: «нервное» приземление → лёгкое сглаживание raw-цели (alpha 0.4–0.5).
- [ ] По фидбеку: латентность NR-восстановления → тюнинг `kSettleFadePerFrame`.
- [ ] После окончания тюнинга: удалить `[GLIDE-DIAG]` по чек-листу из
      `docs/EyeTrackingFoveation_Diagnostics.md`.
- [ ] RU-локализация ключа `foveated_eye_tracking_fov_deadzone` (проверить наличие).
- [ ] Push `feature/dlssnr-vr` на origin (7+ коммитов впереди).
