import { clsx } from "clsx";
import React, { useEffect, useState } from "react";

import styles from "./flag.module.css";

// The flags (flag-icons 7.6.0, MIT): one chunk with every 4:3 SVG, loaded the first time a flag shows. The
// website serves the same files (website/assets/flags/).
const urls = new Map<string, string>();
const loading = new Map<string, Promise<string>>();

function loadFlag(code: string): Promise<string> {
  let p = loading.get(code);
  if (!p) {
    p = import(/* webpackMode: "lazy-once", webpackChunkName: "flags" */ `flag-icons/flags/4x3/${code}.svg`).then(
      (m: { default: string }) => {
        urls.set(code, m.default);
        return m.default;
      },
    );
    loading.set(code, p);
  }
  return p;
}

/** A country's flag, 4:3, `height` px tall; `title` names it on hover. */
export const Flag = React.memo(function Flag({
  code,
  height = 15,
  title,
  className,
}: {
  code: string;
  height?: number;
  title?: string;
  className?: string;
}) {
  const [url, setUrl] = useState(() => urls.get(code));
  useEffect(() => {
    let live = true;
    setUrl(urls.get(code));
    loadFlag(code)
      .then((u) => live && setUrl(u))
      .catch(() => undefined);
    return () => {
      live = false;
    };
  }, [code]);
  const style = { height, width: (height * 4) / 3 };
  return url ? (
    <img className={clsx(styles.flag, className)} src={url} alt={title ?? ""} title={title} style={style} />
  ) : (
    <span className={clsx(styles.flag, className)} style={style} title={title} />
  );
});

/** No country: a dashed 4:3 outline the flag's size. */
export const NoFlag = ({ height = 15, className }: { height?: number; className?: string }) => (
  <span className={clsx(styles.noFlag, className)} style={{ height, width: (height * 4) / 3 }} />
);
