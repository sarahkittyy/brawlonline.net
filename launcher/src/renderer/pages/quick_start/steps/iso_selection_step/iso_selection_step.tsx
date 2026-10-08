import { PRODUCT_NAME } from "@common/product";
import { IsoValidity } from "@common/types";
import { css } from "@emotion/react";
import styled from "@emotion/styled";
import Box from "@mui/material/Box";
import Button from "@mui/material/Button";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback } from "react";
import { useDropzone } from "react-dropzone";

import { ProgressBar } from "@/components/loading_screen/loading_screen";
import { useIsoVerificationFraction } from "@/lib/hooks/use_iso_verification";
import { useIsoPath } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { hasBorder } from "@/styles/has_border";

import { QuickStartHeader } from "../../step_container";
import { findNativeFile, getIsoFileKind, ISO_STEP_EXTENSIONS } from "./iso_file";
import { IsoSelectionStepMessages as Messages } from "./iso_selection_step.messages";

const getColor = (props: any, defaultColor = "#eeeeee") => {
  if (props.isDragAccept) {
    return "#00e676";
  }
  if (props.isDragActive) {
    return "var(--accent-primary)";
  }
  return defaultColor;
};

const Container = styled.div`
  flex: 1;
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  padding: 20px;
  ${(props) =>
    hasBorder({
      width: 25,
      color: getColor(props),
      radius: 25,
      dashOffset: 50,
    })}
  color: var(--off-white);
  outline: none;
  transition: border 0.24s ease-in-out;
  p {
    text-align: center;
    font-weight: 500;
  }
`;

export const IsoSelectionStep = () => {
  const { showError } = useToasts();
  const [tempIsoPath, setTempIsoPath] = React.useState("");
  const validIsoPathQuery = useQuery({
    queryKey: ["validIsoPathQuery", tempIsoPath],
    queryFn: async () => {
      if (!tempIsoPath) {
        return {
          path: tempIsoPath,
          valid: IsoValidity.UNVALIDATED,
        };
      }
      return window.electron.common.checkValidIso(tempIsoPath);
    },
    enabled: Boolean(tempIsoPath),
  });

  const loading = validIsoPathQuery.isLoading;
  // Hashing the image takes ~25 s the first time: show how far along it is.
  const verifyFraction = useIsoVerificationFraction(tempIsoPath);
  const [, setIsoPath] = useIsoPath();
  const nativeFilesRef = React.useRef<File[] | null>(null);

  const chooseIsoPath = (filePath: string) => {
    if (loading || !filePath) {
      return;
    }
    switch (getIsoFileKind(filePath)) {
      case "7z":
        showError(Messages.sevenZFilesMustBeUncompressed());
        return;
      case "compressed":
        showError(Messages.rvzFilesAreIncompatible(PRODUCT_NAME));
        return;
    }
    setTempIsoPath(filePath);
  };

  const onDrop = (acceptedFiles: File[]) => {
    if (loading || acceptedFiles.length === 0) {
      return;
    }

    // Use the validated file from react-dropzone, through its native-backed twin from the drop event
    // (see onDropCapture below). A file that did not come from a drop is used as it is.
    const accepted = acceptedFiles[0];
    const isoFile = findNativeFile(accepted, nativeFilesRef.current) ?? accepted;
    nativeFilesRef.current = null;
    chooseIsoPath(window.electron.utils.getFilePath(isoFile));
  };

  // Select opens the native file dialog, as Settings > Game does: it gives the path directly and
  // filters by extension on every OS.
  const onSelect = async () => {
    const result = await window.electron.common.showOpenDialog({
      properties: ["openFile"],
      filters: [{ name: "Brawl ISO", extensions: [...ISO_STEP_EXTENSIONS] }],
    });
    if (result.canceled || result.filePaths.length === 0) {
      return;
    }
    chooseIsoPath(result.filePaths[0]);
  };

  const validIsoPath = validIsoPathQuery.data?.valid ?? IsoValidity.UNVALIDATED;

  const { getRootProps, getInputProps, isDragActive, isDragAccept, isDragReject } = useDropzone({
    accept: {
      "application/octet-stream": [".iso", ".wbfs", ".rvz"],
      "application/x-7z-compressed": [".7z"],
    },
    onDrop,
    multiple: false,
    noClick: true,
    noKeyboard: true,
  });

  const invalidIso = Boolean(tempIsoPath) && !loading && validIsoPath === IsoValidity.INVALID;
  const onConfirm = useCallback(() => {
    setIsoPath(tempIsoPath).catch(showError);
  }, [showError, setIsoPath, tempIsoPath]);

  React.useEffect(() => {
    if (invalidIso) {
      showError(Messages.providedIsoWillNotWork(PRODUCT_NAME));
    }
  }, [showError, invalidIso]);

  React.useEffect(() => {
    // Auto-confirm ISO if it's valid
    if (validIsoPath === IsoValidity.VALID) {
      onConfirm();
    }
  }, [onConfirm, validIsoPath]);

  return (
    <Box display="flex" flexDirection="column" flexGrow="1" maxWidth="800px" marginLeft="auto" marginRight="auto">
      <div
        css={css`
          margin-bottom: 20px;
        `}
      >
        <QuickStartHeader>{Messages.selectMeleeIso()}</QuickStartHeader>
        <div>{Messages.thisApplicationUsesNtsc()}</div>
      </div>
      <Container
        {...getRootProps({
          isDragActive,
          isDragAccept,
          isDragReject,
          onDropCapture: (e) => {
            // This is a hack to get the native file object from the drag event.
            // In newer Chromium/Electron versions, the drag data store can become protected
            // after the drop handling phase, so by the time onDrop fires, event.dataTransfer.files is empty.
            // The reliable fix is to intercept the native drop event before react-dropzone processes it.
            // We need to do this to get the full file path.
            nativeFilesRef.current = Array.from(e.dataTransfer?.files ?? []);
          },
        })}
      >
        <input {...getInputProps()} />
        {!loading && (
          <Button color="primary" variant="contained" onClick={() => void onSelect().catch(showError)}>
            {Messages.select()}
          </Button>
        )}
        <p>{loading ? Messages.verifyingIso() : Messages.orDragAndDropHere()}</p>
        {loading && verifyFraction !== null && <ProgressBar current={verifyFraction} total={1} />}
      </Container>
    </Box>
  );
};
