import type { SvgIconProps } from "@mui/material/SvgIcon";
import SvgIcon from "@mui/material/SvgIcon";

// Lucide 0.460.0 "clapperboard", https://lucide.dev, ISC License, Copyright (c) Lucide Contributors 2022
// (portions Copyright (c) Cole Bemis 2013-2022, MIT). Its four paths are joined into one, so a
// translucent colour doesn't paint darker where they cross, and a play triangle is added to the board.
// Stroked, at the weight of MUI's outlined icons beside it.
export const ClapperboardIcon = (props: SvgIconProps) => (
  <SvgIcon viewBox="0 0 24 24" {...props}>
    <path
      d="M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3ZM6.2 5.3l3.1 3.9M12.4 3.4l3.1 4M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"
      fill="none"
      stroke="currentColor"
      strokeWidth={2}
      strokeLinecap="round"
      strokeLinejoin="round"
    />
    <path d="M10.5 13.6v4.8l3.8-2.4Z" stroke="currentColor" strokeWidth={1} strokeLinejoin="round" />
  </SvgIcon>
);
