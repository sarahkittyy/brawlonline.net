import styled from "@emotion/styled";

// Slippi's home page footer links its Bluesky, Discord and donation page; those are
// dropped. The plain footer bar used by the replay pages stays.
export const BasicFooter = styled.div`
  display: flex;
  padding: 0 20px;
  height: 50px;
  white-space: nowrap;
  align-items: center;
  background-color: var(--surface-footer);
  font-size: 14px;
  color: var(--surface-3);

  svg {
    width: 20px;
    height: auto;
  }
`;
