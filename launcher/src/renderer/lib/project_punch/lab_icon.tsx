// Project Punch's lab icon (a lab bench with a round flask, a conical flask and a test tube), after
// the picture Sarah picked, drawn on Material's 24 grid with its outlined icons' weight so it sits
// with the header's icons (Home, Replays...). Strokes, so it takes the text colour.
import type { SvgIconProps } from "@mui/material/SvgIcon";
import SvgIcon from "@mui/material/SvgIcon";

export const LabIcon = (props: SvgIconProps) => (
  <SvgIcon viewBox="0 0 24 24" {...props}>
    <g fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round">
      {/* bench: top, legs, crossbar */}
      <path d="M2 13.6H22M4 13.6V21M20 13.6V21M4 17.3H20" />
      {/* round flask */}
      <circle cx="5.6" cy="9.5" r="2.5" />
      {/* the wavy tube from it to the conical flask */}
      <path d="M5.6 7V3.6Q5.6 2.4 6.8 2.4Q7.8 2.4 8.4 3.2Q9.1 4 9.8 3.2Q10.5 2.4 11.2 3.2Q11.9 4 12.6 3.2V5.4" />
      {/* conical flask */}
      <path d="M11.6 5.4H13.6M11.6 5.4V7.5L9.7 10.8Q9.2 11.9 10.4 11.9H14.8Q16 11.9 15.5 10.8L13.6 7.5V5.4" />
      {/* test tube */}
      <path d="M18.2 3.4V9.6A1.3 1.3 0 0 0 20.8 9.6V3.4Z" />
    </g>
  </SvgIcon>
);
