const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');

const runtime = process.env.ACTIONS_RUNTIME_TOKEN;
const claims = JSON.parse(Buffer.from(runtime.split('.')[1], 'base64url'));
const scope = claims.scp.split(' ').find(value => value.startsWith('Actions.Results:'));
const [, workflowRunBackendId, workflowJobRunBackendId] = scope.split(':');
const name = `client-input-${process.env.CLIENT}`;
const transferSize = process.env.TRANSFER_SIZE;
const receiptRecipient = process.env.RECEIPT_RECIPIENT;
const root = process.env.CASE_DIR;

if (!/^\d{8}$/.test(transferSize) || !/^age1[023456789acdefghjklmnpqrstuvwxyz]{58}$/.test(receiptRecipient)) {
  throw new Error('Unexpected candidate transfer inputs');
}

async function artifact(method, fields) {
  const endpoint = new URL(`/twirp/github.actions.results.api.v1.ArtifactService/${method}`, process.env.ACTIONS_RESULTS_URL);
  const response = await fetch(endpoint, {
    method: 'POST',
    headers: { Authorization: `Bearer ${runtime}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ workflowRunBackendId, workflowJobRunBackendId, name, ...fields }),
    signal: AbortSignal.timeout(20000),
  });
  if (!response.ok) {
    return { ok: false, status: response.status };
  }
  return response.json();
}

async function bridge() {
  if (process.env.TRANSFER_MODE === 'offer') {
    const result = await artifact('CreateArtifact', {
      version: 4,
      expiresAt: new Date(Date.now() + 86400000).toISOString(),
    });
    const uploadUrl = result.signedUploadUrl ?? result.signed_upload_url;
    if (!result.ok || !uploadUrl) {
      throw new Error(`Cannot reserve transfer artifact (HTTP ${result.status ?? 'unknown'})`);
    }
    const recipient = fs.readFileSync(path.join(root, 'recipient.txt'), 'utf8').trim();
    const offer = JSON.stringify({ recipient, uploadUrl, transferSize });
    const encrypted = spawnSync('age', ['-r', receiptRecipient, '-o', path.join(root, 'receipt', 'offer.age')], {
      input: offer,
      stdio: ['pipe', 'ignore', 'ignore'],
    });
    if (encrypted.status !== 0) {
      throw new Error('Cannot encrypt the scoped upload receipt');
    }
    return;
  }

  let result;
  for (let attempt = 0; attempt < 30; attempt++) {
    result = await artifact('FinalizeArtifact', { size: transferSize });
    if (result.ok) {
      return;
    }
    await delay(10000);
  }
  throw new Error(`Candidate transfer did not arrive (HTTP ${result.status ?? 'unknown'})`);
}

bridge().catch(error => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});
