# UI Design System — FPV MultiTool

**Source of truth for all web-UI markup.** Если что-то не описано здесь — не выдумывать,
а либо попросить добавить компонент в систему, либо использовать ближайший подходящий из существующих.

Live reference: открой плату, перейди в **System → Styleguide** (визуальный showcase
каждого компонента в работе с code-snippet рядом).

---

## Layout

| Class | Где | Что делает |
|---|---|---|
| `.app-shell`        | `<body>`-wrapper | max-width 1400px, центрирование, padding |
| `.app-header`       | top of shell  | header bar (title + workspace nav + chrome) |
| `.tab-content`      | inside tab    | auto-fit grid `minmax(360px, 1fr)`, gap 16px |
| `.page-section`     | inside tab    | header + body, для группировки cards |
| `.stack` / `.stack-sm` / `.stack-lg` | utility | flex column with gap |
| `.cluster`          | utility       | flex row with gap, wraps |
| `.grid` / `.grid-3` | utility       | 2 / 3 equal columns |
| `.toolbar`          | top of action | actions row (buttons + spacer + status) |

---

## Cards & sections

### Card (default container)

```html
<div class="card">
  <div class="card-header">
    <span class="card-title">Title</span>
    <span class="badge badge-info">optional</span>
    <button class="btn-ghost btn-sm" style="margin-left: auto;">Action</button>
  </div>
  <!-- body: kv rows, fields, banners, etc -->
</div>
```

Variants: `.card.elevated` (shadow), `.card.compact` (smaller padding).

**Don't write `<h2>` / `<h3>`** for card titles — use `.card-title` so visual is consistent.

### Page section (groups multiple cards under a heading)

```html
<div class="page-section">
  <div class="page-section-header">
    <span class="page-section-title">Big section title</span>
    <span class="page-section-subtitle">optional context</span>
  </div>
  <!-- one or more cards inside -->
</div>
```

---

## Action picker — primary entry point

The big decision-cards at the top of every tab. Each card = one user goal.
Replaces "feature dump" pages from v1.

```html
<div class="action-picker">
  <div class="action-card active" onclick="pickAction('read')">
    <span class="action-card-icon">🔋</span>
    <span class="action-card-title">Read battery</span>
    <span class="action-card-desc">Cells, voltage, %, temp, cycles, MAC.</span>
  </div>
  <div class="action-card" onclick="pickAction('reset')">…</div>
</div>
```

Then for each action:

```html
<div id="action-read"><!-- visible default --></div>
<div id="action-reset" style="display:none"><!-- hidden initially --></div>
```

Toggle via `pickAction(name)` (per-tab JS):

```js
function pickAction(name) {
  document.querySelectorAll('.action-card').forEach(c => c.classList.remove('active'));
  event.currentTarget.classList.add('active');
  ['read','reset','probe',...].forEach(a => {
    const el = document.getElementById('action-' + a);
    if (el) el.style.display = (a === name) ? '' : 'none';
  });
}
```

**When NOT to use action-picker:** if a tab has 1 clear single goal (RC Sniff, OTA),
skip it — render the single workflow directly.

---

## Key-value rows (data display)

Replaces the deprecated `.row + .label + .value` triple.

```html
<div class="kv">
  <span class="kv-label">Voltage</span>
  <span class="kv-value mono">14.82 V</span>
</div>
```

Modifiers on `.kv-value`:
- `.mono` — monospace + smaller (для hex / addresses / IP)

Use `.kv` inside `.card`. Multiple `.kv` rows stack with subtle separators.

---

## Stat tiles (top-row dashboards)

Big single-number displays. Use 4-6 in a row at top of tab content.

```html
<div class="stat-grid">
  <div class="stat-tile success">
    <div class="stat-tile-value">14.82V</div>
    <div class="stat-tile-label">Voltage</div>
  </div>
  <div class="stat-tile warn">…</div>
  <div class="stat-tile danger">…</div>
</div>
```

Color modifiers: `.success` / `.warn` / `.danger` (apply to value text color).

---

## Buttons

```html
<button>Default (primary)</button>
<button class="secondary">Secondary (outlined)</button>
<button class="ghost">Ghost (transparent)</button>
<button class="danger">Danger</button>
<button class="success">Success</button>
<button class="btn-sm">Small</button>
<button class="btn-lg">Large</button>
<button disabled>Disabled</button>
<button class="mode-blocked">Mode-blocked (grey + dashed)</button>
```

**Hierarchy rule:**
- Per card / form: ONE primary button, one or two secondary, optional ghost.
- Destructive action: **danger** + always wrap in confirm modal.

---

## Form fields

```html
<div class="field">
  <span class="field-label">SSID</span>
  <input type="text" placeholder="WiFi name">
  <span class="field-help">Optional helper text</span>
</div>
```

Inputs (`<input>`, `<select>`, `<textarea>`) don't need extra classes — base CSS already
applies tokens (border, padding, focus ring).

---

## Banners (contextual messages)

```html
<div class="banner banner-info">Informational</div>
<div class="banner banner-warn">Warning</div>
<div class="banner banner-error">Error / destructive notice</div>
<div class="banner banner-success">Success / confirmation</div>
```

Use **above** the destructive action it warns about. Don't stack 3 banners — pick
the most relevant.

---

## Badges (small inline labels)

```html
<span class="badge badge-success">OK</span>
<span class="badge badge-warn">UNSEALED</span>
<span class="badge badge-danger">FAIL</span>
<span class="badge badge-info">DJI Mavic 3</span>
<span class="badge badge-neutral">128 KB</span>
```

For status indicators with a "live LED" feel, prepend `.dot`:

```html
<span class="badge badge-success"><span class="dot on"></span>I2C alive</span>
```

`.dot` variants: default (subtle), `.on` (pulsing glow green), `.warn`, `.danger`.

---

## Stepper (multi-step destructive flows)

```html
<div class="stepper">
  <div class="step done">1. Connect</div>
  <div class="step done">2. UNSEAL</div>
  <div class="step active">3. Write IT</div>
  <div class="step">4. SEAL</div>
  <div class="step">5. Verify</div>
</div>
```

Use for guided flows with multiple HTTP calls + verification (battery cycle reset,
RX flash, OTA update). User sees progress.

---

## Connection rail (sticky footer per tab)

Bottom-of-tab bar showing what hardware/state is currently active. Single-line.

```html
<div class="connection-rail">
  <span class="dot on"></span>
  <span class="kv-label">Port B</span>
  <span class="kv-value">UART @ GP10/11</span>
  <span class="connection-rail-spacer"></span>
  <span class="connection-rail-action">Owner: <code>crsf_service</code> · 420000 baud</span>
</div>
```

---

## Empty state (placeholder for not-yet-loaded / empty / WIP)

```html
<div class="empty-state">
  <div class="empty-state-icon">🔍</div>
  <div class="empty-state-title">Probe unknown battery</div>
  <div class="empty-state-hint">Click <b>Start scan</b> to enumerate SBS registers.</div>
</div>
```

---

## Tokens — never write hex directly

### Color (semantic)
- Brand: `--primary` `--primary-hover` `--primary-soft`
- Status: `--success` `--warn` `--danger` `--info` (each has a `-soft` variant for backgrounds)
- Surface: `--bg` `--bg-subtle` `--card` `--card-elevated` `--input-bg`
- Border: `--border` `--border-subtle`
- Text: `--text` `--text-muted` `--text-subtle`
- All have light AND dark theme override in `data/style.css`.

### Spacing (4px base, never inline)
`--s-1` (4px) · `--s-2` (8) · `--s-3` (12) · `--s-4` (16) · `--s-5` (20) ·
`--s-6` (24) · `--s-8` (32) · `--s-10` (40)

### Type scale
`--text-xs` (11px) · `--text-sm` (13) · `--text-base` (14) · `--text-md` (15) ·
`--text-lg` (17) · `--text-xl` (20) · `--text-2xl` (24) · `--text-3xl` (32)

### Radius
`--r-sm` (4) · `--r` (6) · `--r-md` (8) · `--r-lg` (12) · `--r-full` (pill)

### Motion
`--t-fast` (120ms) · `--t` (180ms) · `--t-slow` (240ms)

---

## Anti-patterns ❌

- `style="color:#888"` или любой inline-style в табах (кроме `style="display:none"` для
  начального скрытия). Lint валит build.
- Hex колор вне `data/style.css` (lint валит build).
- `<h2>`, `<h3>` вне `.card-header` / `.page-section-header`.
- Изобретать новые tokens (`--my-color`, `--big-padding`) — extend system, не override.
- `.row` / `.label` / `.value` в новом коде — это legacy aliases, использовать `.kv-*`.
- Дублировать button styles inline. Если нужен новый вариант — добавить класс в CSS.
- Разные emoji размеры в action-cards. Все `.action-card-icon` = 28px.

---

## Recipe: новая вкладка с goal-driven IA

```html
<div id="tab-mytab" class="tab-content" style="display:none">

  <!-- 1. Optional: top mode banner / safety warning -->
  <div class="banner banner-warn">⚠ Critical context.</div>

  <!-- 2. Action picker (skip if single goal) -->
  <div class="action-picker">
    <div class="action-card active" onclick="pickMytab('foo')">
      <span class="action-card-icon">🎯</span>
      <span class="action-card-title">Foo</span>
      <span class="action-card-desc">What this does.</span>
    </div>
    <div class="action-card" onclick="pickMytab('bar')">…</div>
  </div>

  <!-- 3. Action panels — one visible at a time -->
  <div id="mytab-foo">
    <div class="toolbar">
      <button>Primary action</button>
      <button class="secondary">Snapshot</button>
      <span class="toolbar-spacer"></span>
      <span class="badge badge-success"><span class="dot on"></span>connected</span>
    </div>

    <div class="stat-grid"><!-- top-row tiles --></div>

    <div class="tab-content" style="margin-top: var(--s-4);">
      <div class="card">
        <div class="card-header"><span class="card-title">Identity</span></div>
        <div class="kv"><span class="kv-label">…</span><span class="kv-value mono">…</span></div>
      </div>
      <!-- more cards -->
    </div>
  </div>

  <div id="mytab-bar" style="display:none">…</div>

  <!-- 4. Connection rail (always at bottom) -->
  <div class="connection-rail">
    <span class="dot on"></span>
    <span class="kv-label">Port B</span>
    <span class="kv-value">UART @ GP10/11</span>
    <span class="connection-rail-spacer"></span>
    <span class="connection-rail-action">…</span>
  </div>
</div>
```

JS handler in `data/ui.js`:

```js
function pickMytab(name) {
  document.querySelectorAll('#tab-mytab .action-card').forEach(c => c.classList.remove('active'));
  event.currentTarget.classList.add('active');
  ['foo','bar'].forEach(a => {
    const el = document.getElementById('mytab-' + a);
    if (el) el.style.display = (a === name) ? '' : 'none';
  });
}
```

---

## Workflow для новой фичи

1. Идея → открыть **mockup/** в браузере, скопировать ближайший шаблон в `mockup/<name>.html`
2. Итерировать визуально (`file://`, без OTA)
3. Когда устраивает — перенести markup в `data/tabs/<name>.html` (с реальными DOM IDs)
4. Подключить handlers в `data/ui.js`
5. `pio run` → `lint_ui.py` проверит:
   - inline styles
   - hardcoded colors
   - forbidden tags
   - undefined tokens
   Регрессии не пройдут.
6. OTA upload → smoke test
7. Открыть **System → Styleguide** на плате, удостовериться что все компоненты
   твоей вкладки выглядят как в showcase

---

## Когда хочется добавить новый компонент

1. Сначала проверить — точно нет уже существующего? (`grep` по `data/style.css` +
   `data/_extras.css` + UI.md)
2. Если правда нужен — добавить в `data/style.css` под подходящим разделом
3. Документировать в UI.md (этот файл) с code-snippet
4. Добавить пример в `data/tabs/_styleguide.html`
5. Только потом использовать в фиче

Не наоборот. Иначе через полгода у нас опять 11 вариантов "warning red".
