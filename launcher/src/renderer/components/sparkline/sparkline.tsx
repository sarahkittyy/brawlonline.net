/**
 * A number's recent history as a line (the rating after each of the last ranked sets), its latest value marked in the
 * accent. Nothing for fewer than two values.
 */
export const Sparkline = ({
  values,
  width = 120,
  height = 36,
  className,
}: {
  values: number[];
  width?: number;
  height?: number;
  className?: string;
}) => {
  if (values.length < 2) {
    return null;
  }
  const min = Math.min(...values);
  const max = Math.max(...values);
  const span = max - min || 1;
  const pts = values.map((v, i) => [(i / (values.length - 1)) * width, height - 3 - ((v - min) / span) * (height - 6)]);
  const d = pts.map(([x, y], i) => `${i ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`).join(" ");
  const [lx, ly] = pts[pts.length - 1];
  return (
    <svg
      className={className}
      width={width}
      height={height}
      viewBox={`0 0 ${width} ${height}`}
      style={{ overflow: "visible", flex: "none" }}
      aria-hidden={true}
      data-testid="sparkline"
    >
      <path d={d} fill="none" stroke="var(--text-dim)" strokeWidth={1.5} />
      <circle cx={lx} cy={ly} r={3} fill="var(--accent-primary)" />
    </svg>
  );
};
