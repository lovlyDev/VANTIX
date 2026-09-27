import { useEffect, useRef } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";

export default function RateChart({ values }: { values: number[] }) {
  const host = useRef<HTMLDivElement>(null);
  const chart = useRef<uPlot | null>(null);
  useEffect(() => {
    if (!host.current) return;
    const width = Math.max(1, host.current.clientWidth);
    chart.current = new uPlot({
      width,
      height: 155,
      cursor: { show: false },
      legend: { show: false },
      scales: { x: { time: false }, y: { auto: true,
        range: (_chart, _min, max) => [0, Math.max(1, max * 1.15)] } },
      axes: [{ show: false }, { show: false }],
      series: [{}, { stroke: "#4f98ff", width: 2 }]
    }, [values.map((_, i) => i), values], host.current);
    const resize = new ResizeObserver(([entry]) => {
      const nextWidth = Math.round(entry.contentRect.width);
      if (nextWidth > 0) chart.current?.setSize({ width: nextWidth, height: 155 });
    });
    resize.observe(host.current);
    return () => { resize.disconnect(); chart.current?.destroy(); chart.current = null; };
  }, []);
  useEffect(() => {
    chart.current?.setData([values.map((_, i) => i), values]);
  }, [values]);
  return <div className="rate-chart" ref={host} role="img" aria-label="График скорости поиска адресов" />;
}
