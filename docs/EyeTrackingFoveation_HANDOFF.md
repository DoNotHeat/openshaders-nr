# Eye Tracking Foveation — статус работы (handoff для нового контекста)

_Обновлено: 2026-09-08. Ветка `feature/dlssnr-vr`, HEAD `5cf7f7dd`._
_Основной гайд: `docs/EyeTrackingFoveation_Implementation_Guide.md`, краткий: `docs/EyeTrackingFoveation_QUICKREF.md`._

---

## 1. Что это за задача

Eye-tracking-driven foveated rendering для Skyrim VR с DLSS:
Высококачественный DLSS-субрект (~1202x993 из 4184x3256 на глаз) следует за взглядом
(трекер Pimax Dream, драйвер sboys3, OpenVR IVRSystem_026 через GetGenericInterface).
Остальная область рендерится дёшево. Neural Rendering (NGX Feature 18) работает
поверх DLSS внутри того же субректа.

## 2. Что уже сделано (закоммичено)

| Коммит | Что |
|---|---|
| `89b54dba` | Интеграция LDR neural rendering |
| `635033c2` | NGX Feature 18 renderer (DLSS NR) |
| `96fcc697` | Screenshot финального VR-композита |
| `3d6748f1` | Батчинг VR-оценки NR и fence-ожиданий |
| `091bfb4d` | Стабилизация flat-пререквизитов |
| `05e037ca` | Flat pass до UI |
| `b013ed45` | Eye tracking базис: per-eye NDC gaze, subrect-офсеты, диагностика `[EYETRACK]`/`[FOVEATED-DIAG]` |
| `5cf7f7dd` | Pinhole-репроекция: gaze-офсет subrect (NDC) вшивается в проекционные матрицы для Streamline (curr = текущий офсет, prev = `Streamline::prevGazeOffsetNDC[2]`) → DLSS репроецирует историю при движении субректа, reset не нужен. Результат: «намного лучше, но не идеально» |

## 3. Незакоммиченные правки (текущее состояние)

Рабочая копия содержит ~115 добавленных / 38 удалённых строк в 5 файлах
(+ новый заголовок `src/Features/Upscaling/EyeTrackingFoveationImpl.h`, не в git).
Смысловой блок — **стабилизация региона и NR при движении взгляда**:

### 3.1 Deadzone + dwell вместо сырого порога (`FoveatedRender.cpp/.h`)

- **Проблема:** человеческий взгляд НИКОГДА не неподвижен (fixation drift,
  микросаккады) + дрейф трекера. Любой малый порог пробивался каждые несколько
  секунд → субрект полз постоянно → DLSS/NR истории постоянно перестраивались → мерцание.
- **Решение, 2 слоя:**
  1. **Deadzone** (`eyeTrackingFovDeadzonePx`, 50–800, дефолт 300, заменяет
     `eyeTrackingFovMoveThresholdPx`): регион перецентрируется только при дрейфе
     gaze НА эту дистанцию ОТ ЗАКРЕПЛЁННОЙ точки.
  2. **Dwell** (`kGazeDwellFrames = 8`): дрейф должен превышать deadzone
     `kGazeDwellFrames` кадров ПОДРЯД (`gazeOverThresholdFrames[2]`), чтобы
     транзиентные скачки (конвергенция, глитчи трекера) не двигали регион.
- После старта глайда: флаг `subrectMovedThisFrame` ставится только пока шаг глайда
  > 2 px (NR-пауза не висит на субпиксельном хвосте); dwell-счётчики реармятся
  против НОВОЙ позиции с 2x deadzone (регион не перезапускается, пока gaze не
  уйдёт далеко от только что применённого офсета).
- Настройка в UI переименована: «FOV Move Threshold» → «FOV Deadzone»
  (ключ локализации `foveated_eye_tracking_fov_deadzone`).

### 3.2 NR: fade интенсивности вместо паузы (`NeuralRendering/Integration.cpp`)

- **Старое (B1):** skip NR пока `subrectMovedThisFrame` + 4 кадра, при
  возобновлении `forceReset` в Feature 18. Проблема: on/off-переключение читается
  как видимое мерцание NR.
- **Новое:** NR остаётся ВКЛЮЧЁННЫМ каждый кадр; его Intensity плавно easing к 0,
  пока субрект движется (`kMoveFadePerFrame = 0.25`), и обратно к 1, когда
  остановился (`kSettleFadePerFrame = 0.08`). Механизм: статический
  `nrIntensityScale` → `GetTuning(settings, intensityScale)` умножает
  `neuralRenderingIntensity`.

### 3.3 NR: re-crop гайдов вместо скипа stale-кадра (`Integration.cpp`)

- **Старое:** если depth/mvec гайды на кадр старше (вырезаны по СТАРОЙ позиции
  субректа) и субрект двинулся — skip NR-оценки (читалось как флик «stale output
  held for a frame» на каждом шаге глайда).
- **Новое:** прямо в NR-пути гайды пере-вырезаются (`CopySubresourceRegion` из
  full SBS depth `kMAIN` и `motionVectorCopyTexture` в
  `FoveatedRenderImpl::Core::vrSubrectDepth/MotionVectors[eye]`) по текущим
  per-eye UV-боксам (`leftUV`/`rightUV` субрект-контроллера, SBS-офсет
  `eye ? eyeWidthIn : 0`), `neuralGuidesFrame = frame`. Цвет и гайды выровнены
  каждый кадр, скип исчез.

### 3.4 `forceReset` параметр в `Renderer::ApplyStereo` (`Renderer.cpp/.h`)

- Сигнатура `ApplyStereo(..., const Tuning&, bool forceReset = false)` — проброс
  до `Runtime::Instance().Execute(..., resetPending[eyeIndex] || forceReset)`.

### 3.5 Новый файл `EyeTrackingFoveationImpl.h` (untracked)

- Выделенный заголовок (судя по использованию
  `FoveatedRenderImpl::Core::vrSubrectDepth` и т.п. в Integration.cpp) —
  вынесенные внутренности foveation-имплементации. **Проверить, что он полностью
  самодостаточен и нужен, перед коммитом.**

## 4. На чём остановились

1. **Последний игровой лог** (`src/CommunityShaders.log`, ~23:16): DLSS и NR
   writeback стабильны, per-eye UV согласованы (leftUV одинаковый для обоих глаз
   в diag — при конвергенции правый глаз отличается, диаг печатает leftUV для обоих).
   Позиции субректа меняются эпизодически (раз в 5 сек в сэмплах diag) — deadzone
   работает, регион не ползёт постоянно.
2. **Субъективный статус до этих правок:** «намного лучше, но не идеально»
   (после pinhole-репроекции DLSS). Новые правки (deadzone+dwell, NR fade,
   re-crop гайдов) написаны, но **в игре ещё не проверены пользователем** —
   это следующий шаг: собрать (`BuildDevFast.bat` / DLL-only деплой), дать
   пользователю потестировать в HMD.
3. Мержить/коммитить текущий блок только после вердикта пользователя.

## 5. Что дальше (кандидаты)

- [ ] Сборка и игровой тест незакоммиченного блока (SE + VR).
- [ ] Тюнинг констант по фидбеку: `eyeTrackingFovDeadzonePx` (300),
      `kGazeDwellFrames` (8), `kMoveFadePerFrame` (0.25), `kSettleFadePerFrame`
      (0.08), порог «meaningful step» 2 px.
- [ ] Коммит блока (ожидаемый тип: `fix(upscaling):` — стабилизация, без
      заявленного перфа) — не забыть ключ локализации и сериализацию
      (NLOHMANN уже обновлён на `eyeTrackingFovDeadzonePx`; у старых пользователей
      ini-значение `eyeTrackingFovMoveThresholdPx` молча отбросится).
- [ ] `src/Features/Upscaling/EyeTrackingFoveationImpl.h` — решить: в git или
      контент уже покрыт существующими заголовками.
- [ ] Проверить миграцию настроек / пресетов Upscaling (ClampSettings уже правлен).

## 6. Ключевые технические факты (не потерять)

- `sl::float4x4 = float4 row[4]`, аксессоров `_14/_24` НЕТ → `m[0].w`, `m[1].w`.
- DLSS пишет в `kMAIN`, NR работает с `kTOTAL`; eyeWidth=4184, размеры совпадают.
- «Два квадрата» в HMD = бинокулярное двоение → per-eye офсеты из СВОЕГО gaze
  каждого глаза (left.x ≠ right.x при конвергенции — норма).
- Gaze-хистерезис обязателен: каждое движение субректа сбрасывает DLSS/NR
  temporal history (мерцание). Pinhole-репроекция через матрицы решает это для
  DLSS; для NR — fade интенсивности.
- Лучшие настройки пользователя (до deadzone): FOV Move Threshold 60,
  Glide Factor 0.05 (минимум). EMA gaze зафиксирован 0.2; Gaze Threshold/Smoothing
  удалены как мёртвые.
- Диагностика: `[EYETRACK]`, `[FOVEATED-DIAG]`, `[DLSSNR-DIAG]` в
  `C:\Users\ausho\Documents\My Games\Skyrim VR\SKSE\CommunityShaders.log`
  (читать самому — разрешено).
- Сборка: VS 18 (toolset 14.51), MSB8074 warning про *.module.json безвреден
  (битые JSON в `build\ALL\CommunityShaders.dir\Release` — удалить).
  Warnings-as-errors: C4100 = ошибка. `get_errors` в VS Code не доверять для
  .cpp — только реальные ошибки MSBuild.
- Опасно: `multi_replace_string_in_file` на этом проекте дважды повреждал файл
  (терялись присваивания/скобки). После каждой правки — перечитать блок.
