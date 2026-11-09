/**
 * M1-P policy probe provider.
 *
 * Purpose: capture, from the real Pi request path, the exact tool set that Pi
 * declared to the model, without any network access, credential, or tool run.
 *
 * Loaded explicitly by tools/check-pi-policy.ps1 with:
 *     pi -ne -e <this file> --provider pi-policy-faux --model pi-policy-model ...
 *
 * The provider is the installed `faux` provider (no HTTP, no credentials): it
 * answers every request with the fixed text `POLICY_OK`. Its response factory
 * reads the already-normalized TranscriptContext, replays the transcript tool
 * declarations with getCurrentTools() - the same helper the built-in providers
 * use to build their wire tool list - and writes them to the file named by
 * PI_POLICY_CAPTURE_FILE. Nothing else is touched.
 *
 * This file is not part of build.bat: the regular fake-RPC regression suite
 * still runs without an installed Pi.
 */

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import {
	fauxAssistantMessage,
	fauxProvider,
	getCurrentTools,
	type FauxResponseFactory,
	type Tool,
	type TranscriptContext,
} from "@earendil-works/pi-ai";

/** Provider id used by tools/check-pi-policy.ps1 on the command line. */
export const POLICY_PROVIDER_ID = "pi-policy-faux";
/** Model id used by tools/check-pi-policy.ps1 on the command line. */
export const POLICY_MODEL_ID = "pi-policy-model";
/** Fictional api id; only dispatches inside this provider, never on the wire. */
export const POLICY_API_ID = "pi-policy-faux-api";
/** Fixed assistant text; proves the request really reached this provider. */
export const POLICY_REPLY = "POLICY_OK";
/** Env var naming the fixture JSON to write. Set only by the checker script. */
export const POLICY_CAPTURE_ENV = "PI_POLICY_CAPTURE_FILE";

const CAPTURE_VERSION = 1;

interface CaptureDeclaration {
	name: string;
	description: string;
	parameters: unknown;
}

interface CaptureRequest {
	index: number;
	toolCount: number;
	toolNames: string[];
	declarations: CaptureDeclaration[];
	messageCount: number;
	messageRoles: string[];
}

interface CaptureFile {
	captureVersion: number;
	provider: string;
	model: string;
	api: string;
	capturedAt: string;
	requestCount: number;
	toolCount: number;
	toolNames: string[];
	requests: CaptureRequest[];
}

function capturePath(): string {
	const file = process.env[POLICY_CAPTURE_ENV];
	if (!file) {
		// Fail loudly: a silent empty capture would look like a hardened pass.
		throw new Error(`${POLICY_CAPTURE_ENV} is not set; the policy probe cannot record tool declarations`);
	}
	return file;
}

function toDeclaration(tool: Tool): CaptureDeclaration {
	let parameters: unknown = null;
	try {
		parameters = JSON.parse(JSON.stringify(tool.parameters ?? null));
	} catch {
		parameters = "<unserializable>";
	}
	return {
		name: tool.name,
		description: typeof tool.description === "string" ? tool.description : "",
		parameters,
	};
}

function readPreviousRequests(file: string): CaptureRequest[] {
	try {
		const parsed = JSON.parse(readFileSync(file, "utf8")) as Partial<CaptureFile>;
		return Array.isArray(parsed.requests) ? (parsed.requests as CaptureRequest[]) : [];
	} catch {
		// Missing or partial file: start a fresh record.
		return [];
	}
}

/**
 * Record the current model-facing tool declarations and return the request index.
 * The whole observed set is kept so a multi-request run cannot hide a transient
 * declaration behind a later empty one.
 */
function recordCapture(context: TranscriptContext): CaptureRequest {
	const file = capturePath();
	const tools = getCurrentTools(context.messages);
	const requests = readPreviousRequests(file);
	const entry: CaptureRequest = {
		index: requests.length + 1,
		toolCount: tools.length,
		toolNames: tools.map((tool) => tool.name),
		declarations: tools.map(toDeclaration),
		messageCount: context.messages.length,
		messageRoles: context.messages.map((message) => message.role),
	};
	requests.push(entry);

	const payload: CaptureFile = {
		captureVersion: CAPTURE_VERSION,
		provider: POLICY_PROVIDER_ID,
		model: POLICY_MODEL_ID,
		api: POLICY_API_ID,
		capturedAt: new Date().toISOString(),
		requestCount: requests.length,
		toolCount: entry.toolCount,
		toolNames: entry.toolNames,
		requests,
	};

	mkdirSync(dirname(file), { recursive: true });
	writeFileSync(file, `${JSON.stringify(payload, null, 2)}\n`, "utf8");
	return entry;
}

export default function piPolicyProvider(pi: ExtensionAPI): void {
	const faux = fauxProvider({
		provider: POLICY_PROVIDER_ID,
		api: POLICY_API_ID,
		models: [
			{
				id: POLICY_MODEL_ID,
				name: "Pi Policy Probe",
				reasoning: false,
				input: ["text"],
				contextWindow: 128000,
				maxTokens: 4096,
			},
		],
	});

	// Keep answering: a retry must not turn into "no more faux responses queued",
	// which would be misread as a policy failure by the checker.
	const respond: FauxResponseFactory = (context) => {
		recordCapture(context);
		faux.appendResponses([respond]);
		return fauxAssistantMessage(POLICY_REPLY);
	};

	faux.setResponses([respond]);
	pi.registerProvider(faux.provider);
}
