import type { ButtonProps } from "@mui/material/Button";
import MatButton from "@mui/material/Button";

// The theme's contained button: with game frames it is the frame image with light text, so no colours here
// (a dark label and a primary-coloured icon disappeared into the green frame).
export const Button = ({ children, ...rest }: ButtonProps) => {
  return (
    <MatButton
      variant="contained"
      sx={{
        fontWeight: 500,
        fontSize: 12,
      }}
      {...rest}
    >
      {children}
    </MatButton>
  );
};
