// The changelog popup (docs/design/changelog.md): what's new in this release, Sarah's own Markdown from
// the changelog editor, shown once after an update. The X (or Esc) closes it for good; a click beside
// it doesn't (a click in the launcher as it opens would lose it).
// The text's rules are also the editor's preview's (website/changelog/write/changelog.css): change both.
import styled from "@emotion/styled";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import Dialog from "@mui/material/Dialog";
import DialogContent from "@mui/material/DialogContent";
import IconButton from "@mui/material/IconButton";

import { bodyFont, titleFont } from "@/styles/with_font";

import { ChangelogMessages as Messages } from "./changelog.messages";

export type ChangelogDialogViewProps = {
  open: boolean;
  /** Cleaned HTML (changelogHtml). */
  html: string;
  onClose: () => void;
};

export const ChangelogDialogView = ({ open, html, onClose }: ChangelogDialogViewProps) => (
  <Dialog
    open={open}
    onClose={(_e, reason) => reason !== "backdropClick" && onClose()}
    fullWidth={true}
    scroll="paper"
    PaperProps={{ style: { maxWidth: 600 } }}
  >
    <IconButton
      aria-label={Messages.close()}
      onClick={onClose}
      size="small"
      style={{ position: "absolute", right: 12, top: 12, zIndex: 1 }}
      data-testid="changelog-close"
    >
      <CloseRoundedIcon fontSize="small" />
    </IconButton>
    <DialogContent style={{ padding: "20px 24px 24px" }}>
      <Body dangerouslySetInnerHTML={{ __html: html }} data-testid="changelog-body" />
    </DialogContent>
  </Dialog>
);

const Body = styled.div`
  font-family: ${bodyFont};
  font-size: 15px;
  line-height: 1.55;
  padding-right: 24px;
  user-select: text;

  * {
    user-select: text;
  }

  & > :first-child {
    margin-top: 0;
  }

  & > :last-child {
    margin-bottom: 0;
  }

  h2 {
    margin: 22px 0 8px;
    font-family: ${titleFont};
    font-size: 22px;
    font-weight: 700;
    line-height: 1.25;
  }

  h3,
  h4 {
    margin: 18px 0 6px;
    font-family: ${titleFont};
    font-size: 17px;
    font-weight: 700;
  }

  p {
    margin: 0 0 10px;
  }

  ul,
  ol {
    margin: 0 0 10px;
    padding-left: 22px;
  }

  li {
    margin: 4px 0;
  }

  a {
    color: inherit;
    text-decoration: underline;
    word-break: break-word;
  }

  code {
    padding: 1px 4px;
    border-radius: 3px;
    background: rgba(255, 255, 255, 0.08);
    font: 0.9em ui-monospace, "Cascadia Mono", Consolas, monospace;
  }

  pre {
    margin: 0 0 10px;
    padding: 10px 12px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.06);
    overflow: auto;
  }

  pre code {
    padding: 0;
    background: none;
  }

  blockquote {
    margin: 0 0 10px;
    padding: 2px 12px;
    border-left: 3px solid rgba(255, 255, 255, 0.25);
    opacity: 0.85;
  }

  hr {
    margin: 16px 0;
    border: 0;
    border-top: 1px solid rgba(255, 255, 255, 0.15);
  }

  figure {
    margin: 14px 0;
  }

  img,
  video {
    display: block;
    max-width: 100%;
    height: auto;
    margin: 0 auto;
    border-radius: 4px;
  }

  p img {
    display: inline;
    vertical-align: middle;
  }

  figcaption {
    margin-top: 6px;
    font-size: 13px;
    text-align: center;
    opacity: 0.7;
  }

  table {
    margin: 0 0 10px;
    border-collapse: collapse;
  }

  th,
  td {
    padding: 4px 8px;
    border: 1px solid rgba(255, 255, 255, 0.15);
  }
`;
