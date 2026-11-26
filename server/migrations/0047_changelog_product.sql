-- EmissaryRecomp's patch notes beside the launcher's (docs/design/changelog.md, "EmissaryRecomp"):
-- the agent board publishes them once a recomp release is live (`pp-admin changelog publish
-- --emissary v0.7.2`). Launchers whose Play uses EmissaryRecomp ask for them with the launcher's
-- (`GET /v1/changelogs?emissary=1`) and show both in the one popup; the others never see them.
ALTER TABLE changelogs
    ADD COLUMN product text NOT NULL DEFAULT 'launcher' CHECK (product IN ('launcher', 'emissary')),
    -- An Emissary one's release (`v0.7.2`); NULL for the launcher's.
    ADD COLUMN version text;
