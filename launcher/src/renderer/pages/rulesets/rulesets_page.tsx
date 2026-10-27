import type { RulesetDraft, RulesetView } from "@accounts/types";
import type { Ruleset } from "@common/rulesets";
import {
  newRuleset,
  normalizeRuleset,
  picksUsed,
  rulesetFromJson,
  rulesetProblem,
  rulesetToJson,
} from "@common/rulesets";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import FileDownloadOutlinedIcon from "@mui/icons-material/FileDownloadOutlined";
import FileUploadOutlinedIcon from "@mui/icons-material/FileUploadOutlined";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import RefreshIcon from "@mui/icons-material/Refresh";
import SearchIcon from "@mui/icons-material/Search";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { clsx } from "clsx";
import log from "electron-log";
import React, { useCallback, useEffect, useRef, useState } from "react";

import { ConfirmationModal } from "@/components/confirmation_modal/confirmation_modal";
import { InfiniteScrollContainer } from "@/components/infinite_scroll_container/infinite_scroll_container";
import { PagedListStatus } from "@/components/paged_list_status/paged_list_status";
import { useAccount } from "@/lib/hooks/use_account";
import { useCursorPages } from "@/lib/hooks/use_cursor_pages";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { rulesetTitle, useRememberedRuleset } from "@/lib/rulesets/use_remembered_ruleset";
import { useServices } from "@/services";

import { RulesetDetails } from "./ruleset_details";
import { RulesetEditor } from "./ruleset_editor";
import { RulesetsPageMessages as Messages } from "./rulesets_page.messages";
import styles from "./rulesets_page.module.css";

/** What the editor holds: a saved draft (`draftId`), or a ruleset not saved yet. */
type Editing = {
  draftId?: string;
  /** The published version this edits. */
  basedOn: string | null;
  ruleset: Ruleset;
  dirty: boolean;
};

export const myRulesetsKey = (uid: string | undefined) => ["myRulesets", uid] as const;
export const PUBLIC_RULESETS_KEY = "publicRulesets";
/** How long the search waits after the last keystroke. */
const SEARCH_DEBOUNCE_MS = 300;

/** "Bo3 · strikes 1-2-1 · 2 bans", "No set · loser picks". */
export function setSummary(r: Ruleset): string {
  if (r.bestOf > 0) {
    const parts = [Messages.summaryBestOf(r.bestOf), Messages.summaryStrikes(r.strikes.join("-"))];
    if (r.bans > 0) {
      parts.push(Messages.summaryBans(r.bans));
    }
    if (r.alternateStages) {
      parts.push(Messages.summaryAlternateStages());
    }
    return parts.join(" · ");
  }
  const choice =
    r.stageChoice === "strike"
      ? Messages.summaryStrikes(r.strikes.join("-"))
      : r.stageChoice === "random"
      ? Messages.summaryRandom()
      : r.stageChoice === "loser-picks-list"
      ? Messages.summaryLoserPicksList()
      : Messages.summaryLoserPicks();
  const alts = r.alternateStages && picksUsed(r) ? ` · ${Messages.summaryAlternateStages()}` : "";
  return `${Messages.summaryNoSet()} · ${choice}${alts}`;
}

/**
 * The Rulesets page (docs/rulesets-protocol.md §4), one column top to bottom: the account's drafts, its published
 * rulesets (hide, edit as a new version, copy the link, use for rooms), then every public ruleset with a search and
 * infinite scrolling (the user's decision, 2026-10-12: rulesets are all public). A published or public row opens its
 * read-only preview; a draft opens in the editor. Both lists refresh with the button, when the page opens and when
 * the window comes back into focus. Also import / export of ruleset files.
 */
export const RulesetsPage = React.memo(function RulesetsPage() {
  const { backendService } = useServices();
  const uid = useAccount((s) => s.user?.uid);
  const openLogin = useLoginModal((s) => s.openModal);
  const { showError, showSuccess } = useToasts();
  const queryClient = useQueryClient();
  const remembered = useRememberedRuleset();

  const [editing, setEditing] = useState<Editing | null>(null);
  const [pendingSwitch, setPendingSwitch] = useState<(() => void) | null>(null);
  const [deleting, setDeleting] = useState<RulesetDraft | null>(null);
  const [justPublished, setJustPublished] = useState<RulesetView | null>(null);
  const [showHidden, setShowHidden] = useState(false);
  const [busy, setBusy] = useState<"save" | "publish" | null>(null);
  /** The published ruleset (the account's or anyone's) shown read-only while nothing is being edited. */
  const [viewing, setViewing] = useState<RulesetView | null>(null);
  const [search, setSearch] = useState("");
  const [searchFor, setSearchFor] = useState("");
  const fileInput = useRef<HTMLInputElement>(null);

  const query = useQuery({
    queryKey: myRulesetsKey(uid),
    queryFn: () => backendService.fetchMyRulesets(),
    enabled: uid != null,
    retry: false,
  });
  const refresh = () => queryClient.invalidateQueries({ queryKey: myRulesetsKey(uid) });

  useEffect(() => {
    const t = setTimeout(() => setSearchFor(search.trim()), SEARCH_DEBOUNCE_MS);
    return () => clearTimeout(t);
  }, [search]);
  const fetchPublic = useCallback(
    async (after: string | undefined) => {
      const page = await backendService.fetchPublicRulesets(searchFor, after);
      return { items: page.rulesets, next: page.next };
    },
    [backendService, searchFor],
  );
  const pub = useCursorPages<RulesetView>({
    queryKey: [PUBLIC_RULESETS_KEY, searchFor],
    fetchPage: fetchPublic,
    enabled: uid != null,
  });

  // Fresh lists when the page opens, on the refresh button and when the window comes back into focus (rulesets
  // published from another machine show up without restarting).
  const refreshAll = useCallback(() => {
    void queryClient.invalidateQueries({ queryKey: myRulesetsKey(uid) });
    void queryClient.invalidateQueries({ queryKey: [PUBLIC_RULESETS_KEY] });
  }, [queryClient, uid]);
  useEffect(() => {
    refreshAll();
    window.addEventListener("focus", refreshAll);
    return () => window.removeEventListener("focus", refreshAll);
  }, [refreshAll]);

  if (!uid) {
    return (
      <div className={styles.centered}>
        <LockOutlinedIcon style={{ fontSize: 48 }} />
        <p>{Messages.loggedOut()}</p>
        <Button color="secondary" variant="outlined" onClick={openLogin}>
          {Messages.logIn()}
        </Button>
      </div>
    );
  }

  /** Opens something in the editor, asking first if unsaved changes would be lost. */
  const open = (next: Editing | null, view: RulesetView | null = null) => {
    const go = () => {
      setEditing(next);
      setViewing(view);
    };
    if (editing?.dirty) {
      setPendingSwitch(() => go);
    } else {
      go();
    }
  };

  const save = async (): Promise<RulesetDraft | null> => {
    if (!editing) {
      return null;
    }
    setBusy("save");
    try {
      const draft = await backendService.saveRulesetDraft(
        { ruleset: editing.ruleset, basedOn: editing.basedOn },
        editing.draftId,
      );
      setEditing({ draftId: draft.id, basedOn: draft.basedOn, ruleset: draft.ruleset, dirty: false });
      await refresh();
      return draft;
    } catch (err) {
      showError(err);
      return null;
    } finally {
      setBusy(null);
    }
  };

  const publish = async () => {
    if (!editing) {
      return;
    }
    setBusy("publish");
    try {
      // A saved draft is published as itself (and goes away); anything else as its values.
      let draftId = editing.draftId;
      if (draftId && editing.dirty) {
        const draft = await backendService.saveRulesetDraft(
          { ruleset: editing.ruleset, basedOn: editing.basedOn },
          draftId,
        );
        draftId = draft.id;
      }
      const view = await backendService.publishRuleset(
        draftId ? { draftId } : { ruleset: editing.ruleset, basedOn: editing.basedOn },
      );
      setEditing(null);
      setJustPublished(view);
      await refresh();
    } catch (err) {
      showError(err);
    } finally {
      setBusy(null);
    }
  };

  const copy = (text: string) => {
    void navigator.clipboard.writeText(text).then(() => showSuccess(Messages.copied()));
  };

  const exportFile = (r: Ruleset) => {
    const blob = new Blob([rulesetToJson(r)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    const base = r.name.replace(/[^A-Za-z0-9 _.-]+/g, "").trim() || "ruleset";
    a.download = r.version > 0 ? `${base} v${r.version}.json` : `${base}.json`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
  };

  const importFile = async (file: File) => {
    try {
      const ruleset = rulesetFromJson(await file.text());
      open({ basedOn: null, ruleset, dirty: true });
    } catch (err) {
      showError(Messages.importFailed(err instanceof Error ? err.message : String(err)));
    }
  };

  const toggleHidden = async (view: RulesetView) => {
    try {
      await backendService.setRulesetHidden(view.code, !view.hidden);
      refreshAll();
    } catch (err) {
      showError(err);
    }
  };

  const rememberForRooms = async (code: string) => {
    try {
      await remembered.setRemembered(code);
    } catch (err) {
      showError(err);
    }
  };

  const data = query.data;
  const published = data?.published ?? [];
  const hiddenCount = published.filter((p) => p.hidden).length;
  const shownPublished = showHidden ? published : published.filter((p) => !p.hidden);
  const problem = editing ? rulesetProblem(editing.ruleset) : null;
  // One of the account's own: the fresh copy from its list (hidden may have changed).
  const ownViewed = viewing ? published.find((p) => p.code === viewing.code) : undefined;
  const viewed = ownViewed ?? viewing;

  return (
    <div className={styles.page}>
      <div className={styles.listColumn}>
        <div className={styles.header}>
          <Typography variant="h5">{Messages.rulesets()}</Typography>
          <div className={styles.headerActions}>
            <Tooltip title={Messages.refresh()}>
              <IconButton
                size="small"
                onClick={refreshAll}
                aria-label={Messages.refresh()}
                data-testid="refresh-rulesets"
              >
                <RefreshIcon fontSize="small" />
              </IconButton>
            </Tooltip>
            <Button
              size="small"
              variant="contained"
              color="secondary"
              disableElevation={true}
              startIcon={<AddRoundedIcon />}
              onClick={() => open({ basedOn: null, ruleset: newRuleset(), dirty: true })}
              data-testid="new-ruleset"
              sx={{ textTransform: "none" }}
            >
              {Messages.newRuleset()}
            </Button>
            <Button
              size="small"
              color="secondary"
              startIcon={<FileUploadOutlinedIcon />}
              onClick={() => fileInput.current?.click()}
              sx={{ textTransform: "none" }}
            >
              {Messages.importFile()}
            </Button>
            <input
              ref={fileInput}
              type="file"
              accept=".json,application/json"
              hidden={true}
              onChange={(e) => {
                const file = e.target.files?.[0];
                e.target.value = "";
                if (file) {
                  void importFile(file);
                }
              }}
            />
          </div>
        </div>
        <Typography variant="body2" color="text.secondary" className={styles.intro}>
          {Messages.intro()}
        </Typography>

        <InfiniteScrollContainer
          className={styles.lists}
          onLoadMore={pub.loadMore}
          hasMore={pub.hasMore && !pub.error}
          isLoading={pub.isLoading || pub.isLoadingMore}
        >
          {query.isLoading ? (
            <div className={styles.inlineStatus}>
              <CircularProgress color="inherit" size={20} />
            </div>
          ) : query.isError ? (
            <div className={styles.inlineStatus}>
              <span>{Messages.loadFailed()}</span>
              <Button color="secondary" onClick={() => void query.refetch()}>
                {Messages.retry()}
              </Button>
            </div>
          ) : (
            <>
              <Typography variant="subtitle2" className={styles.listTitle}>
                {Messages.drafts()}
              </Typography>
              {data?.drafts.length ? (
                data.drafts.map((d) => (
                  <div
                    key={d.id}
                    className={clsx(styles.item, editing?.draftId === d.id && styles.selected)}
                    data-draft={d.id}
                  >
                    <button
                      type="button"
                      className={styles.itemMain}
                      onClick={() => open({ draftId: d.id, basedOn: d.basedOn, ruleset: d.ruleset, dirty: false })}
                    >
                      <span className={styles.itemName}>{d.ruleset.name || Messages.editorNew()}</span>
                      <span className={styles.itemMeta}>
                        {d.basedOn && Messages.editorEditing(d.basedOn)}
                        {d.problem && <span className={styles.warn}>{Messages.notReady()}</span>}
                      </span>
                    </button>
                    <Button
                      size="small"
                      color="secondary"
                      sx={{ textTransform: "none" }}
                      onClick={() => setDeleting(d)}
                    >
                      {Messages.delete()}
                    </Button>
                  </div>
                ))
              ) : (
                <Typography variant="body2" color="text.secondary" className={styles.empty}>
                  {Messages.noDrafts()}
                </Typography>
              )}

              <div className={styles.listTitleRow}>
                <Typography variant="subtitle2" className={styles.listTitle}>
                  {Messages.published()}
                </Typography>
                {hiddenCount > 0 && (
                  <Button
                    size="small"
                    color="secondary"
                    sx={{ textTransform: "none" }}
                    onClick={() => setShowHidden(!showHidden)}
                  >
                    {showHidden ? Messages.hideHidden() : Messages.showHidden(hiddenCount)}
                  </Button>
                )}
              </div>
              {shownPublished.length ? (
                shownPublished.map((p) => (
                  <PublishedRow
                    key={p.code}
                    view={p}
                    inUse={remembered.ruleset?.code === p.code}
                    selected={!editing && viewing?.code === p.code}
                    onView={() => open(null, p)}
                    onEdit={() => open({ basedOn: p.code, ruleset: { ...p.ruleset }, dirty: false })}
                    onCopy={copy}
                    onExport={() => exportFile(p.ruleset)}
                    onToggleHidden={() => void toggleHidden(p)}
                    onUse={() => void rememberForRooms(p.code)}
                  />
                ))
              ) : (
                <Typography variant="body2" color="text.secondary" className={styles.empty}>
                  {Messages.noPublished()}
                </Typography>
              )}
            </>
          )}

          <Typography variant="subtitle2" className={styles.listTitle}>
            {Messages.publicRulesets()}
          </Typography>
          <TextField
            size="small"
            hiddenLabel={true}
            fullWidth={true}
            value={search}
            placeholder={Messages.searchPlaceholder()}
            onChange={(e) => setSearch(e.target.value)}
            inputProps={{ "aria-label": Messages.searchPlaceholder(), "data-testid": "public-search", maxLength: 64 }}
            InputProps={{
              startAdornment: (
                <InputAdornment position="start">
                  <SearchIcon fontSize="small" />
                </InputAdornment>
              ),
            }}
            sx={{ mb: 1 }}
          />
          {pub.items.map((p) => (
            <PublicRow
              key={p.code}
              view={p}
              inUse={remembered.ruleset?.code === p.code}
              selected={!editing && viewing?.code === p.code}
              onView={() => open(null, p)}
              onCopy={copy}
              onUse={() => void rememberForRooms(p.code)}
            />
          ))}
          {!pub.isLoading && !pub.error && pub.items.length === 0 && (
            <Typography variant="body2" color="text.secondary" className={styles.empty}>
              {searchFor ? Messages.noMatches(searchFor) : Messages.noPublic()}
            </Typography>
          )}
          <PagedListStatus pages={pub} />
        </InfiniteScrollContainer>
      </div>

      {!editing && viewed && (
        <div className={styles.editorColumn} data-testid="ruleset-view">
          <div className={styles.editorHeader}>
            <Typography variant="h6">{rulesetTitle(viewed)}</Typography>
            <CodeCopy code={viewed.code} onCopy={copy} className={styles.headerCode} />
            {viewed.hidden && <span className={styles.unsaved}>{Messages.hiddenLabel()}</span>}
          </div>
          <div className={styles.editorScroll}>
            <RulesetDetails ruleset={viewed.ruleset} />
          </div>
          <div className={styles.editorFooter}>
            <div className={styles.viewActions}>
              <Button color="secondary" sx={{ textTransform: "none" }} onClick={() => copy(viewed.link)}>
                {Messages.copyLink()}
              </Button>
              {remembered.ruleset?.code === viewed.code ? (
                <span className={styles.inUse}>{Messages.inUse()}</span>
              ) : (
                <Button
                  color="secondary"
                  sx={{ textTransform: "none" }}
                  onClick={() => void rememberForRooms(viewed.code)}
                  data-testid="view-use"
                >
                  {Messages.forMyRooms()}
                </Button>
              )}
            </div>
            <div className={styles.footerButtons}>
              <Button color="secondary" onClick={() => setViewing(null)}>
                {Messages.close()}
              </Button>
              {ownViewed && (
                <Button color="secondary" onClick={() => void toggleHidden(ownViewed)}>
                  {ownViewed.hidden ? Messages.show() : Messages.hide()}
                </Button>
              )}
              {ownViewed && (
                <Button
                  color="primary"
                  variant="contained"
                  disableElevation={true}
                  onClick={() => open({ basedOn: ownViewed.code, ruleset: { ...ownViewed.ruleset }, dirty: false })}
                >
                  {Messages.editAsNewVersion()}
                </Button>
              )}
            </div>
          </div>
        </div>
      )}

      {editing && (
        <div className={styles.editorColumn} data-testid="ruleset-editor">
          <div className={styles.editorHeader}>
            <Typography variant="h6">
              {editing.basedOn
                ? Messages.editorEditing(editing.basedOn)
                : editing.draftId
                ? Messages.editorDraft()
                : Messages.editorNew()}
            </Typography>
            {editing.dirty && <span className={styles.unsaved}>{Messages.unsaved()}</span>}
          </div>
          <div className={styles.editorScroll} data-testid="ruleset-editor-scroll">
            <RulesetEditor
              value={editing.ruleset}
              onChange={(ruleset) => setEditing({ ...editing, ruleset, dirty: true })}
            />
          </div>
          <div className={styles.editorFooter}>
            <span className={clsx(styles.problem, !problem && styles.ok)} data-testid="ruleset-problem">
              {problem ?? Messages.readyToPublish()}
            </span>
            <div className={styles.footerButtons}>
              <Tooltip title={Messages.export()}>
                <IconButton size="small" onClick={() => exportFile(normalizeRuleset(editing.ruleset))}>
                  <FileDownloadOutlinedIcon fontSize="small" />
                </IconButton>
              </Tooltip>
              <Button color="secondary" onClick={() => open(null)}>
                {Messages.close()}
              </Button>
              <Button
                color="secondary"
                variant="outlined"
                disabled={busy !== null || (!editing.dirty && editing.draftId !== undefined)}
                onClick={() => void save()}
                data-testid="save-ruleset"
              >
                {Messages.save()}
              </Button>
              <Button
                color="primary"
                variant="contained"
                disableElevation={true}
                disabled={busy !== null || problem !== null}
                onClick={() => void publish()}
                startIcon={busy === "publish" ? <CircularProgress color="inherit" size={14} /> : undefined}
                data-testid="publish-ruleset"
              >
                {busy === "publish" ? Messages.publishing() : Messages.publish()}
              </Button>
            </div>
          </div>
        </div>
      )}

      <ConfirmationModal
        open={pendingSwitch !== null}
        onClose={() => setPendingSwitch(null)}
        onSubmit={() => pendingSwitch?.()}
        title={Messages.discardTitle()}
        confirmText={Messages.discard()}
      >
        {Messages.discardBody()}
      </ConfirmationModal>

      <ConfirmationModal
        open={deleting !== null}
        onClose={() => setDeleting(null)}
        onSubmit={() => {
          const d = deleting;
          if (!d) {
            return;
          }
          backendService
            .deleteRulesetDraft(d.id)
            .then(() => {
              if (editing?.draftId === d.id) {
                setEditing(null);
              }
              return refresh();
            })
            .catch((err) => {
              log.error(err);
              showError(err);
            });
        }}
        title={Messages.deleteDraftTitle()}
        confirmText={Messages.delete()}
      >
        {deleting && Messages.deleteDraftBody(deleting.ruleset.name)}
      </ConfirmationModal>

      <Dialog open={justPublished !== null} onClose={() => setJustPublished(null)} fullWidth={true} maxWidth="xs">
        {justPublished && (
          <>
            <DialogTitle>
              {Messages.publishedTitle()}: {rulesetTitle(justPublished)}
            </DialogTitle>
            <DialogContent>
              <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
                {Messages.publishedBody()}
              </Typography>
              <CopyLine
                label={Messages.code()}
                tooltip={Messages.copyCode()}
                value={justPublished.code}
                onCopy={copy}
                mono={true}
              />
              <CopyLine
                label={Messages.link()}
                tooltip={Messages.copyLink()}
                value={justPublished.link}
                onCopy={copy}
              />
            </DialogContent>
            <DialogActions>
              <Button
                color="secondary"
                onClick={() => {
                  void rememberForRooms(justPublished.code);
                  setJustPublished(null);
                }}
              >
                {Messages.forMyRooms()}
              </Button>
              <Button color="primary" onClick={() => setJustPublished(null)} data-testid="published-done">
                {Messages.done()}
              </Button>
            </DialogActions>
          </>
        )}
      </Dialog>
    </div>
  );
});

export const PublishedRow = ({
  view,
  inUse,
  selected,
  onView,
  onEdit,
  onCopy,
  onExport,
  onToggleHidden,
  onUse,
}: {
  view: RulesetView;
  inUse: boolean;
  selected: boolean;
  onView: () => void;
  onEdit: () => void;
  onCopy: (text: string) => void;
  onExport: () => void;
  onToggleHidden: () => void;
  onUse: () => void;
}) => (
  <div
    className={clsx(styles.item, view.hidden && styles.hiddenItem, selected && styles.selected)}
    data-ruleset={view.code}
  >
    <button type="button" className={styles.itemMain} onClick={onView}>
      <span className={styles.itemName}>
        {view.ruleset.name} <span className={styles.itemVersion}>{Messages.version(view.ruleset.version)}</span>
      </span>
      <span className={styles.itemMeta}>
        <CodeCopy code={view.code} onCopy={onCopy} />
        <RowSummary view={view} />
        {view.hidden && <span>{Messages.hiddenLabel()}</span>}
        {view.newer && <span>{Messages.newerVersion(view.newer.version)}</span>}
        {inUse && <span className={styles.inUse}>{Messages.inUse()}</span>}
      </span>
    </button>
    <div className={styles.itemActions}>
      <Tooltip title={Messages.export()}>
        <IconButton size="small" onClick={onExport} aria-label={Messages.export()}>
          <FileDownloadOutlinedIcon fontSize="inherit" />
        </IconButton>
      </Tooltip>
      {!inUse && (
        <Button size="small" color="secondary" sx={rowButtonSx} onClick={onUse}>
          {Messages.forMyRooms()}
        </Button>
      )}
      <Tooltip title={Messages.editAsNewVersion()}>
        <Button size="small" color="secondary" sx={rowButtonSx} onClick={onEdit}>
          {Messages.edit()}
        </Button>
      </Tooltip>
      <Tooltip title={view.hidden ? "" : Messages.hideTooltip()}>
        <Button size="small" color="secondary" sx={rowButtonSx} onClick={onToggleHidden}>
          {view.hidden ? Messages.show() : Messages.hide()}
        </Button>
      </Tooltip>
    </div>
  </div>
);

export const PublicRow = ({
  view,
  inUse,
  selected,
  onView,
  onCopy,
  onUse,
}: {
  view: RulesetView;
  inUse: boolean;
  selected: boolean;
  onView: () => void;
  onCopy: (text: string) => void;
  onUse: () => void;
}) => (
  <div className={clsx(styles.item, selected && styles.selected)} data-public={view.code}>
    <button type="button" className={styles.itemMain} onClick={onView}>
      <span className={styles.itemName}>
        {view.ruleset.name} <span className={styles.itemVersion}>{Messages.version(view.ruleset.version)}</span>{" "}
        <span className={styles.itemBy}>{Messages.by(view.ruleset.by)}</span>
      </span>
      <span className={styles.itemMeta}>
        <CodeCopy code={view.code} onCopy={onCopy} />
        <RowSummary view={view} />
        {inUse && <span className={styles.inUse}>{Messages.inUse()}</span>}
      </span>
    </button>
    <div className={styles.itemActions}>
      {!inUse && (
        <Button size="small" color="secondary" sx={rowButtonSx} onClick={onUse}>
          {Messages.forMyRooms()}
        </Button>
      )}
    </div>
  </div>
);

/** "No set · random stage · played 1423 times", on one line (cut short only in a very narrow window). */
export const RowSummary = ({ view }: { view: RulesetView }) => {
  const text = `${setSummary(view.ruleset)} · ${Messages.played(view.games)}`;
  return (
    <span className={styles.itemSummary} title={text}>
      {text}
    </span>
  );
};

/** A row's text buttons, without MUI's 64 px minimum width, so the row's text keeps the room. */
const rowButtonSx = { textTransform: "none", minWidth: 0, px: 1 } as const;

/**
 * A ruleset's code with a copy icon to its left: a click on either copies the code. Inside a row's main button it is
 * a span (a button can't hold a button) that keeps the click from opening the ruleset.
 */
export const CodeCopy = ({
  code,
  onCopy,
  className,
}: {
  code: string;
  onCopy: (text: string) => void;
  className?: string;
}) => {
  const copyIt = (e: React.SyntheticEvent) => {
    e.stopPropagation();
    e.preventDefault();
    onCopy(code);
  };
  return (
    <Tooltip title={Messages.copyCode()}>
      <span
        role="button"
        tabIndex={0}
        className={clsx(styles.codeCopy, className)}
        onClick={copyIt}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") {
            copyIt(e);
          }
        }}
        aria-label={`${Messages.copyCode()} ${code}`}
        data-testid="copy-code"
      >
        <ContentCopyIcon className={styles.codeCopyIcon} />
        <span className={styles.code}>{code}</span>
      </span>
    </Tooltip>
  );
};

const CopyLine = ({
  label,
  tooltip,
  value,
  onCopy,
  mono,
}: {
  label: string;
  tooltip: string;
  value: string;
  onCopy: (text: string) => void;
  mono?: boolean;
}) => (
  <div className={styles.copyLine}>
    <span className={styles.copyLabel}>{label}</span>
    <span className={clsx(styles.copyValue, mono && styles.code)} data-testid={`published-${label.toLowerCase()}`}>
      {value}
    </span>
    <Tooltip title={tooltip}>
      <IconButton size="small" onClick={() => onCopy(value)} aria-label={tooltip}>
        <ContentCopyIcon fontSize="inherit" />
      </IconButton>
    </Tooltip>
  </div>
);
