/* /login/ (docs/design/blog.md §4): logs in with BrawlAuth and goes back to ?next=, or says who is
   logged in when there is nowhere to go back to. */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const next = params.has("next") ? BrawlAuth.safeNext(params.get("next")) : null;

  function showDone(s) {
    $("form").hidden = true;
    $("done").hidden = false;
    $("done-who").textContent = `${s.user.displayName}${s.user.connectCode ? ` (${s.user.connectCode})` : ""}, ${s.user.email}`;
    $("done-blog").hidden = !BrawlAuth.hasRole(s, "admin");
  }

  function showForm() {
    $("done").hidden = true;
    $("form").hidden = false;
    $("email").focus();
  }

  function setError(text) {
    $("error").textContent = text || "";
    $("error").hidden = !text;
  }

  // Already logged in: straight back, or say so.
  const existing = BrawlAuth.session();
  if (existing) {
    if (next) {
      location.replace(next);
      return;
    }
    showDone(existing);
    BrawlAuth.refresh().then((s) => (s ? showDone(s) : showForm()));
  }

  $("form").addEventListener("submit", async (e) => {
    e.preventDefault();
    setError("");
    const email = $("email").value.trim();
    const password = $("password").value;
    if (!email || !password) {
      setError("Enter your email and password.");
      return;
    }
    $("submit").disabled = true;
    try {
      const s = await BrawlAuth.login(email, password);
      if (next) location.replace(next);
      else showDone(s);
    } catch (err) {
      setError(err.message);
      $("password").select();
    } finally {
      $("submit").disabled = false;
    }
  });

  $("forgot").addEventListener("click", async () => {
    const email = $("email").value.trim();
    const note = $("reset-note");
    note.hidden = false;
    if (!email) {
      note.textContent = "Type your email above, then click this again: we'll send a link to reset your password.";
      $("email").focus();
      return;
    }
    $("forgot").disabled = true;
    try {
      await BrawlAuth.api("/v1/auth/password-reset/request", { method: "POST", json: { email }, auth: false });
      note.textContent = `If ${email} has an account, a reset link is on its way. Check your inbox.`;
    } catch (err) {
      note.textContent = err.message;
    } finally {
      $("forgot").disabled = false;
    }
  });

  $("logout").addEventListener("click", async () => {
    await BrawlAuth.logout();
    showForm();
  });
})();
