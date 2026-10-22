import type { SvgIconProps } from "@mui/material/SvgIcon";
import SvgIcon from "@mui/material/SvgIcon";

// A film strip with a play triangle and a pencil: render to video. The pencil is Lucide 0.460.0
// "pencil", https://lucide.dev, ISC License, Copyright (c) Lucide Contributors 2022 (portions
// Copyright (c) Cole Bemis 2013-2022, MIT), at half size over the strip's corner, which is left
// open around it. Stroked, as ClapperboardIcon beside it.
export const RenderVideoIcon = (props: SvgIconProps) => (
  <SvgIcon viewBox="0 0 24 24" {...props}>
    <path
      d="M12 19H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v5M2 7h18M6.5 3v4M11 3v4M15.5 3v4M2 15h12.5M6.5 15v4"
      fill="none"
      stroke="currentColor"
      strokeWidth={2}
      strokeLinecap="round"
      strokeLinejoin="round"
    />
    <path d="M8.6 9v4l3.2-2Z" stroke="currentColor" strokeWidth={1} strokeLinejoin="round" />
    <path
      d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"
      transform="translate(11.7 11.7) scale(0.5)"
      fill="none"
      stroke="currentColor"
      strokeWidth={4}
      strokeLinecap="round"
      strokeLinejoin="round"
    />
  </SvgIcon>
);
