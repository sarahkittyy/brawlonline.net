import type { SvgIconProps } from "@mui/material/SvgIcon";
import SvgIcon from "@mui/material/SvgIcon";
import { useId } from "react";

// ClapperboardIcon's clapperboard with a download arrow taped to its corner: render to video. The
// arrow is Lucide 0.460.0 "arrow-down-to-line", https://lucide.dev, ISC License, Copyright (c)
// Lucide Contributors 2022 (portions Copyright (c) Cole Bemis 2013-2022, MIT). The board is at 0.8
// and the arrow at 0.58, their strokes scaled back to 2; the board is cut away around the arrow.
export const RenderVideoIcon = (props: SvgIconProps) => {
  // (Every row has one: each its own mask.)
  const mask = `render-video-${useId().replace(/:/g, "")}`;
  return (
    <SvgIcon viewBox="0 0 24 24" {...props}>
      <defs>
        <mask id={mask} maskUnits="userSpaceOnUse" x="0" y="0" width="24" height="24">
          <rect width="24" height="24" fill="white" />
          <rect x="13.3" y="10.6" width="11" height="14" rx="2" fill="black" />
        </mask>
      </defs>
      <g mask={`url(#${mask})`}>
        <g transform="translate(0 0.6) scale(0.8)">
          <path
            d="M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3ZM6.2 5.3l3.1 3.9M12.4 3.4l3.1 4M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"
            fill="none"
            stroke="currentColor"
            strokeWidth={2.5}
            strokeLinecap="round"
            strokeLinejoin="round"
          />
          <path d="M10.5 13.6v4.8l3.8-2.4Z" stroke="currentColor" strokeWidth={1.25} strokeLinejoin="round" />
        </g>
      </g>
      <path
        d="M12 17V3m-6 8 6 6 6-6M19 21H5"
        transform="translate(11.88 10.72) scale(0.58)"
        fill="none"
        stroke="currentColor"
        strokeWidth={3.45}
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </SvgIcon>
  );
};
