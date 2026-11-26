// Stacked daily bar chart in plain SVG, with a hover tooltip.
// series: [{ key, label, cssVar }], rows: [{ day: 'YYYY-MM-DD', <key>: number }]

const SVG = 'http://www.w3.org/2000/svg';
const el = (name, attrs = {}) => {
  const n = document.createElementNS(SVG, name);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  return n;
};

function niceMax(v) {
  if (v <= 0) return 10;
  const p = 10 ** Math.floor(Math.log10(v));
  const f = v / p;
  return [1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10].find((x) => f <= x) * p;
}

// Path for a bar segment with only the top corners rounded.
function topRounded(x, y, w, h, r) {
  r = Math.min(r, w / 2, h);
  return `M${x},${y + h}V${y + r}Q${x},${y} ${x + r},${y}H${x + w - r}Q${x + w},${y} ${x + w},${y + r}V${y + h}Z`;
}

export function renderStackedBars(container, { rows, series, formatValue, formatDay, onPick, ariaLabel }) {
  container.replaceChildren();
  const width = Math.max(container.clientWidth, 280);
  const height = 260;
  const m = { top: 12, right: 8, bottom: 28, left: 52 };
  const iw = width - m.left - m.right;
  const ih = height - m.top - m.bottom;

  const totals = rows.map((r) => series.reduce((s, x) => s + (r[x.key] || 0), 0));
  const max = niceMax(Math.max(0, ...totals));
  const y = (v) => m.top + ih - (v / max) * ih;

  const svg = el('svg', { viewBox: `0 0 ${width} ${height}`, width, height, role: 'img' });
  svg.setAttribute('aria-label', ariaLabel || 'Daily water usage, stacked by inlet');

  // Grid and y labels
  for (let i = 0; i <= 4; i++) {
    const v = (max / 4) * i;
    const gy = y(v);
    svg.append(el('line', { x1: m.left, x2: width - m.right, y1: gy, y2: gy, class: i ? 'grid' : 'axis' }));
    const t = el('text', { x: m.left - 6, y: gy + 4, 'text-anchor': 'end', class: 'tick' });
    t.textContent = formatValue(v);
    svg.append(t);
  }

  const n = rows.length || 1;
  const step = iw / n;
  const barW = Math.max(2, Math.min(28, step * 0.7));
  const labelEvery = Math.ceil(n / Math.floor(iw / 56));

  const tip = document.createElement('div');
  tip.className = 'tooltip';
  tip.hidden = true;
  container.append(svg, tip);

  rows.forEach((r, i) => {
    const cx = m.left + step * i + step / 2;
    const x = cx - barW / 2;
    let acc = 0;
    const g = el('g', { class: 'bar' });
    const nonZero = series.filter((s) => (r[s.key] || 0) > 0);
    series.forEach((s) => {
      const v = r[s.key] || 0;
      if (v <= 0) return;
      const y0 = y(acc);
      const y1 = y(acc + v);
      acc += v;
      const isTop = s === nonZero[nonZero.length - 1];
      // 2px surface gap between stacked segments
      const h = Math.max(0, y0 - y1 - (acc - v > 0 ? 2 : 0));
      const shape = isTop
        ? el('path', { d: topRounded(x, y1, barW, h, 4) })
        : el('rect', { x, y: y1, width: barW, height: h });
      shape.setAttribute('fill', `var(${s.cssVar})`);
      g.append(shape);
    });
    // Hit target: full column height, wider than the bar
    const hit = el('rect', { x: m.left + step * i, y: m.top, width: step, height: ih, class: 'hit' });
    g.append(hit);
    svg.append(g);

    if (i % labelEvery === 0) {
      const t = el('text', { x: cx, y: height - 8, 'text-anchor': 'middle', class: 'tick' });
      t.textContent = formatDay(r.day, true);
      svg.append(t);
    }

    const show = () => {
      g.classList.add('active');
      const lines = series
        .map((s) => `<div class="tip-row"><span class="swatch" style="background:var(${s.cssVar})"></span>${s.label}<b>${formatValue(r[s.key] || 0)}</b></div>`)
        .join('');
      tip.innerHTML = `<div class="tip-title">${formatDay(r.day)}</div>${lines}${series.length > 1 ? `<div class="tip-row total">Total<b>${formatValue(totals[i])}</b></div>` : ''}`;
      tip.hidden = false;
      // Beside the bar (right if it fits, else left), so it never hides the bar itself.
      const right = cx + barW / 2 + 8;
      const left = right + tip.offsetWidth <= width ? right : Math.max(0, cx - barW / 2 - 8 - tip.offsetWidth);
      tip.style.left = `${left}px`;
      tip.style.top = `${m.top}px`;
    };
    const hide = () => { g.classList.remove('active'); tip.hidden = true; };
    hit.addEventListener('pointerenter', show);
    hit.addEventListener('pointerleave', hide);
    hit.addEventListener('click', () => { show(); if (onPick) onPick(r); });
    if (onPick) hit.style.cursor = 'pointer';
  });
}

// Line chart over one day: x = minutes since midnight (0..1440), y = litres.
// series: [{ label, cssVar, points: [{ t, v }] }] with t ascending.
// The tooltip follows the pointer and shows each line's value at that time.
export function renderDayLines(container, { series, formatValue, formatTime, ariaLabel, endT = 1440 }) {
  container.replaceChildren();
  const width = Math.max(container.clientWidth, 280);
  const height = 260;
  const m = { top: 12, right: 12, bottom: 28, left: 52 };
  const iw = width - m.left - m.right;
  const ih = height - m.top - m.bottom;
  const max = niceMax(Math.max(0, ...series.flatMap((s) => s.points.map((p) => p.v))));
  const x = (t) => m.left + (t / 1440) * iw;
  const y = (v) => m.top + ih - (v / max) * ih;

  const svg = el('svg', { viewBox: `0 0 ${width} ${height}`, width, height, role: 'img' });
  svg.setAttribute('aria-label', ariaLabel || 'Water used during the day');
  for (let i = 0; i <= 4; i++) {
    const v = (max / 4) * i;
    svg.append(el('line', { x1: m.left, x2: width - m.right, y1: y(v), y2: y(v), class: i ? 'grid' : 'axis' }));
    const t = el('text', { x: m.left - 6, y: y(v) + 4, 'text-anchor': 'end', class: 'tick' });
    t.textContent = formatValue(v);
    svg.append(t);
  }
  const every = iw < 420 ? 360 : 180;
  for (let t = 0; t <= 1440; t += every) {
    const lab = el('text', { x: x(t), y: height - 8, 'text-anchor': t === 0 ? 'start' : t === 1440 ? 'end' : 'middle', class: 'tick' });
    lab.textContent = formatTime(t);
    svg.append(lab);
  }

  for (const s of series) {
    if (!s.points.length) continue;
    const d = s.points.map((p, i) => `${i ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`).join('');
    svg.append(el('path', { d, fill: 'none', stroke: `var(${s.cssVar})`, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
  }

  // Hover: vertical guide and a dot per line
  const guide = el('line', { y1: m.top, y2: m.top + ih, class: 'axis', visibility: 'hidden' });
  const dots = series.map((s) => el('circle', { r: 4, fill: `var(${s.cssVar})`, stroke: 'var(--surface)', 'stroke-width': 2, visibility: 'hidden' }));
  svg.append(guide, ...dots);
  const hit = el('rect', { x: m.left, y: m.top, width: iw, height: ih, fill: 'transparent' });
  svg.append(hit);

  const tip = document.createElement('div');
  tip.className = 'tooltip';
  tip.hidden = true;
  container.append(svg, tip);

  const valueAt = (pts, t) => {
    if (!pts.length || t < pts[0].t) return 0;
    for (let i = 1; i < pts.length; i++) {
      if (t <= pts[i].t) {
        const a = pts[i - 1]; const b = pts[i];
        return b.t === a.t ? b.v : a.v + ((b.v - a.v) * (t - a.t)) / (b.t - a.t);
      }
    }
    return pts[pts.length - 1].v;
  };
  const move = (ev) => {
    const r = svg.getBoundingClientRect();
    const px = ((ev.clientX - r.left) / r.width) * width;
    const t = Math.max(0, Math.min(endT, ((px - m.left) / iw) * 1440));
    const vals = series.map((s) => valueAt(s.points, t));
    guide.setAttribute('x1', x(t)); guide.setAttribute('x2', x(t)); guide.setAttribute('visibility', 'visible');
    dots.forEach((c, i) => { c.setAttribute('cx', x(t)); c.setAttribute('cy', y(vals[i])); c.setAttribute('visibility', 'visible'); });
    tip.innerHTML = `<div class="tip-title">Used by ${formatTime(Math.round(t))}</div>` + series
      .map((s, i) => `<div class="tip-row"><span class="swatch" style="background:var(${s.cssVar})"></span>${s.label}<b>${formatValue(vals[i])}</b></div>`).join('') +
      (series.length > 1 ? `<div class="tip-row total">Total<b>${formatValue(vals.reduce((a, b) => a + b, 0))}</b></div>` : '');
    tip.hidden = false;
    const right = x(t) + 12;
    tip.style.left = `${right + tip.offsetWidth <= width ? right : Math.max(0, x(t) - 12 - tip.offsetWidth)}px`;
    tip.style.top = `${m.top}px`;
  };
  const hide = () => { tip.hidden = true; guide.setAttribute('visibility', 'hidden'); dots.forEach((c) => c.setAttribute('visibility', 'hidden')); };
  hit.addEventListener('pointermove', move);
  hit.addEventListener('pointerdown', move);
  hit.addEventListener('pointerleave', hide);
}
