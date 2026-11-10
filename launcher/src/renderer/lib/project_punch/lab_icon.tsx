// Project Punch's lab icon (a lab bench with a round flask, a conical flask and a test tube in a stand),
// redrawn as strokes after the picture Sarah picked (800 px, scaled by 0.03), so it takes the text colour.
import type { SvgIconProps } from "@mui/material/SvgIcon";
import SvgIcon from "@mui/material/SvgIcon";

export const LabIcon = (props: SvgIconProps) => (
  <SvgIcon viewBox="0 0 24 24" {...props}>
    <g fill="none" stroke="currentColor" strokeWidth={1.1} strokeLinecap="round" strokeLinejoin="round">
      {/* bench: top, legs, crossbar */}
      <rect x="0.7" y="11.8" width="22.6" height="1.3" rx="0.35" />
      <path d="M2.65 13.1V23.2M21.35 13.1V23.2M2.65 16.05H21.35" />
      {/* round flask and its neck */}
      <circle cx="5.34" cy="8.64" r="2.76" />
      <path d="M4.45 5.95V4.05H6.25V5.95" />
      {/* the wavy tube from one flask to the other */}
      <path d="M5.34 4.05V1.6Q5.34 1.05 5.9 1.05Q6.6 1.05 7.1 1.6Q7.6 2.15 8.2 1.6Q8.8 1.05 9.4 1.6Q10 2.15 10.6 1.6Q11.2 1.05 11.8 1.6Q12.3 2.05 12.9 1.45Q13.6 1.05 13.6 1.8V3.4" />
      {/* conical flask */}
      <path d="M12.6 5.25V3.4H14.6V5.25M12.7 5.25L11 8.9Q10.75 9.5 11.4 9.5H15.8Q16.45 9.5 16.2 8.9L14.5 5.25Z" />
      {/* stand: rod, arm, foot */}
      <path d="M17.25 3.6V11.8M14.6 4.5H18.95M13.5 10.7V11.8" />
      {/* test tube */}
      <path d="M18.95 2.75V8.8A0.8 0.8 0 0 0 20.55 8.8V2.75Z" />
    </g>
  </SvgIcon>
);
