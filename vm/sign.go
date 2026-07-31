/* ============================================================
   Integrity — verifying a signed .pyro  (roadmap 11.14)

   A signed container carries flag bit 5 and ends with a 32-byte
   HMAC-SHA256 over every byte before it: magic, flags, constants, code,
   the debug section, the assets and the declared permissions.

   WHY THE SIGNATURE COVERS THE PERMISSIONS

   11.12 puts the program's declared permissions inside the container, and the
   VM enforces them. That section is the first thing worth editing on a
   .pyro you did not write — widening `net` costs one byte and no error. It is
   therefore inside what is signed, which is also why the signature is written
   last rather than in the header.

   WHY HMAC AND NOT A PUBLIC-KEY SIGNATURE

   Both ends have to do this with no dependencies: Burnout is Python, this is
   Go, and the project ships neither a crypto library nor a vendored one.
   HMAC-SHA256 is in both standard libraries; Ed25519 is in Go's and not in
   Python's, and hand-rolling one is not something to do on the way past.

   The consequence is honest and worth stating plainly rather than burying:
   this is a SHARED SECRET. Anyone who can verify can also sign. It answers
   "did this file arrive as it was built, by someone holding our key" — the
   distribution and tampering question — and it does NOT let you publish a
   public key so that strangers can verify your builds. That needs asymmetric
   signing and is a separate change.

   WHAT HAPPENS WITHOUT A KEY

   Nothing. An unsigned .pyro runs as it always has, and a signed one runs
   without checking when no key is configured, because most people running a
   .pyro have no key and refusing would make signing unusable. The check is
   opt-in on the VERIFIER's side: set PYRO_KEY (or --key) and the VM will
   refuse anything that does not match.

   And when a key IS configured, an UNSIGNED file is refused too. That is the
   whole point: if unsigned files were accepted under a key, stripping the
   signature would be a complete bypass, and an attacker who can edit the
   bytes can certainly delete 32 of them and clear a flag bit.
   ============================================================ */

package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"os"
	"strings"
)

const flagSigned = 0x20
const sigLen = sha256.Size

// signingKey returns the configured key, or nil when there is none.
// --key=... is handled by main(); PYRO_KEY is the form that works in a
// container or a service manager, where argv is awkward and often logged.
var signingKey []byte

func loadSigningKey(spec string) {
	if spec == "" {
		spec = os.Getenv("PYRO_KEY")
		if spec == "" {
			if f := os.Getenv("PYRO_KEY_FILE"); f != "" {
				spec = "file:" + f
			}
		}
	}
	if spec == "" {
		return
	}
	var key []byte
	if strings.HasPrefix(spec, "file:") {
		b, err := os.ReadFile(spec[5:])
		if err != nil {
			fatal("--key file: " + err.Error())
		}
		key = b
	} else {
		key = []byte(spec)
	}
	// Trimmed on both sides, matching what Burnout does when it reads a key
	// file: an editor's trailing newline must not make signing and verifying
	// disagree about "the same" key.
	key = []byte(strings.TrimSpace(string(key)))
	if len(key) < 16 {
		fatal("the signing key is shorter than 16 bytes; that is almost " +
			"certainly the wrong file")
	}
	signingKey = key
}

// verifySignature returns the program bytes with any signature removed,
// aborting when a configured key does not match.
func verifySignature(data []byte, flags byte) []byte {
	signed := flags&flagSigned != 0

	if signed {
		if len(data) < sigLen+6 {
			fatal("this .pyro claims to be signed but is too short to hold a signature")
		}
	}
	if signingKey == nil {
		// No key configured: strip the signature so the rest of the loader
		// sees the container it expects, and run. Reporting nothing here is
		// deliberate — a warning on every run of a signed file trains people
		// to ignore warnings.
		if signed {
			return data[:len(data)-sigLen]
		}
		return data
	}
	if !signed {
		fatal("a signing key is configured, but this .pyro is not signed.\n" +
			"  Refusing it on purpose: accepting unsigned files under a key would\n" +
			"  make stripping the signature a complete bypass.\n" +
			"  Compile it with:  cryoc app.cryo --backend pyro --sign env:PYRO_KEY")
	}
	body := data[:len(data)-sigLen]
	want := data[len(data)-sigLen:]
	mac := hmac.New(sha256.New, signingKey)
	mac.Write(body)
	// Constant-time: a byte-by-byte compare leaks how much of a forged
	// signature was right, which is enough to construct one a byte at a time.
	if !hmac.Equal(mac.Sum(nil), want) {
		fatal("signature check FAILED for this .pyro.\n" +
			"  The file does not match the key — it was built with a different\n" +
			"  key, or it was modified after it was built. Not running it.")
	}
	return body
}
