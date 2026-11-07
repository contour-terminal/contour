# The Windows code-signing signers, as data. scripts/windows-signing/WindowsSigning.psm1 interprets this table and
# Test-SigningTable checks it; adding a signer or a tool is a new row here, not a change of logic.
#
# A signer signs the executables of the payload (what the installers carry) and then the packages themselves.
#   Transport      None: nothing is signed. SignPath: the SignPath action signs and hands back. Command: a tool is run
#                  per file (the steps below).
#   Payload        Own: only OwnBinaries are signed (a signer that may sign the project's own binaries only).
#                  Unsigned: every portable executable that is not signed already. The `none` signer selects the same
#                  files, so its manifest lists exactly what a real signer would sign (identity rows).
#   AllowedBuilds  Which builds a signer may sign: releases (a release tag), releases-and-master (master pushes too).
#                  Empty: never selectable by CI, only run by hand.
#   Publisher      The subject the signing certificate must have (a -like pattern); checked on every signature a signer
#                  makes or hands back. Empty for the signers that sign nothing real (none, dryrun) and for the local
#                  signtool row, which signs with whatever certificate the owner's store holds.
#   Invocation     InPlace: the tool signs the file it is given. InOut: it reads {in} and writes {out}. Directory: it
#                  signs the files of {inputDir} into {outputDir}.
#   Timestamp      A Command step either uses {timestamp} (and lists TimestampUrls, one per attempt in turn) or says
#                  Timestamp = 'Tool' for a tool that timestamps by itself and cannot rotate.
#
# A tool may carry ProbeArguments (what `tools` runs to see it start; a library has none), an Environment (set for each
# call of the tool, then removed) and Cleanup (paths deleted after each step that used it; an unset {env:X} skips one).
# Neither a .bat nor a .cmd may be a tool's Executable: cmd.exe would parse the arguments, secrets included, again.
#
# Arguments take {file}, {in}, {out}, {inputDir}, {outputDir}, {timestamp}, {env:NAME} (a secret, never printed),
# {tool:NAME} (a tool's path), {toolDir:NAME} (a tool's folder), {data:FILE} (a file beside the module) and
# {product:name} and {product:url}.
@{
    # What a signer limited to the project's own binaries signs.
    OwnBinaries = @('bin/contour.exe')
    # What signatures show as the signed content.
    Product = @{ Name = 'Contour'; Url = 'https://github.com/contour-terminal/contour/' }
    # Per invocation; attempt k uses TimestampUrls[k % n], so a timestamp server that is down is not asked twice.
    Attempts = 3
    RetryDelaySeconds = @(5, 15)
    # The tools, each pinned: a Windows SDK or PATH program, or a release asset of a repository by tag and SHA-256.
    Tools = @{
        signtool     = @{ Source = 'WindowsSdk'; Executable = 'signtool.exe'; ProbeArguments = @('sign', '/?') }
        ssign        = @{ Source = 'GitHubRelease'; Repository = 'Le-Syl21/ssign'; Tag = 'v0.1.10'
                          Asset = 'ssign-windows-x86_64.zip'
                          Sha256 = '68acfc7535830abb716c85a08f28d36dd46ed3098e3cf5c6f0ebe3c8c6694516'; Executable = 'ssign.exe'
                          ProbeArguments = @('--version')
                          # The session a login leaves behind: a bearer token that signs for about 30 minutes.
                          Cleanup = @('{env:TEMP}/ssign', '{env:HOME}/.cache/ssign', '{env:USERPROFILE}/.cache/ssign', '{env:XDG_RUNTIME_DIR}/ssign') }
        ssignPkcs11  = @{ Source = 'GitHubRelease'; Repository = 'Le-Syl21/ssign'; Tag = 'v0.1.10'
                          Asset = 'ssign-pkcs11-windows-x86_64.zip'
                          Sha256 = 'c546a565f174b265c1ab3359fed8e7afb243adacb8113296791d77ac60cac0b6'; Executable = 'ssign_pkcs11.dll'
                          Cleanup = @('{env:TEMP}/ssign', '{env:HOME}/.cache/ssign', '{env:USERPROFILE}/.cache/ssign', '{env:XDG_RUNTIME_DIR}/ssign') }
        osslsigncode = @{ Source = 'GitHubRelease'; Repository = 'mtrojnar/osslsigncode'; Tag = '2.14'
                          Asset = 'osslsigncode-2.14-windows-x64-mingw.zip'
                          Sha256 = '9a1722aaf62a27852c4eb9c35749a0248065052d0ae0a93d4ed6bb49def027f2'; Executable = 'bin/osslsigncode.exe'
                          ProbeArguments = @('--version') }
        libp11       = @{ Source = 'GitHubRelease'; Repository = 'OpenSC/libp11'; Tag = 'libp11-0.4.21'
                          Asset = 'libp11-0.4.21-x64.zip'
                          Sha256 = '443f6ddbf15b437df0e37190c8673e76fa5f7268d22d99052d1e9b2fe1af46fd'; Executable = 'pkcs11prov.dll' }
        # The 214,673,784-byte archive (the release lists no digest; this one was computed from the download) bundles
        # its own JDK, jdk-11.0.2, so no Java is needed on the runner. Its CodeSignTool.bat is never run: it resolves
        # Java relative to the current directory and cmd.exe would parse the credentials in its arguments again. The
        # bundled Java runs the jar directly, with the folder CodeSignTool reads its configuration from in its
        # environment.
        codesigntool = @{ Source = 'GitHubRelease'; Repository = 'SSLcom/CodeSignTool'; Tag = 'v1.3.2'
                          Asset = 'CodeSignTool-v1.3.2-windows.zip'
                          Sha256 = '4afc32e8b7f79bbe1de7e4e7049aaad4e0f754357613b9bbec0e3052f06fd36b'; Executable = 'jdk-11.0.2/bin/java.exe'
                          Environment = @{ CODE_SIGN_TOOL_PATH = '{toolDir}' }
                          ProbeArguments = @('-jar', '{toolDir}/jar/code_sign_tool-1.3.2.jar', '--version') }
    }
    Signers = @{
        none     = @{ Transport = 'None'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master'); Publisher = '' }
        # SignPath Foundation's certificate names the foundation as its subject.
        signpath = @{ Transport = 'SignPath'; Payload = 'Own'; AllowedBuilds = @('releases'); Publisher = 'CN=SignPath Foundation*' }
        # Probed without credentials: osslsigncode 2.14 with libp11's pkcs11prov.dll as the provider and ssign's
        # ssign_pkcs11.dll as the PKCS#11 module loads both, and the module reports that CERTUM_EMAIL is not set, which is
        # the expected refusal. osslsigncode's "Unable to load provider: legacy" is harmless.
        # Publisher: Certum's open-source certificates carry the owner's name in the subject; check it against the first
        # real signature and adjust the pattern.
        certum   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master')
                      Publisher = '*Christian Parpart*'
                      Secrets = @('CERTUM_EMAIL', 'CERTUM_OTP')
                      PayloadStep = @{ Tool = 'ssign'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://time.certum.pl/', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('--timestamp-url', '{timestamp}', '{file}') }
                      PackageStep = @{ Tool = 'osslsigncode'; Invocation = 'InOut'
                                       TimestampUrls = @('http://time.certum.pl/', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('sign', '-provider', '{tool:libp11}', '-pkcs11module', '{tool:ssignPkcs11}',
                                                     '-pkcs11cert', 'pkcs11:type=cert', '-key', 'pkcs11:type=private',
                                                     '-ac', '{data:certum-code-signing-2021-ca.pem}', '-h', 'sha256',
                                                     '-n', '{product:name}', '-i', '{product:url}',
                                                     '-ts', '{timestamp}', '-in', '{in}', '-out', '{out}') } }
        # CodeSignTool's batch_sign signs a directory into another; its help lists -program_name, which the package
        # step passes (the payload's files carry the program name of their own file description). It timestamps through
        # conf/code_sign_tool.properties (TSA_URL=http://ts.ssl.com), so it cannot rotate servers: Timestamp = 'Tool'.
        sslcom   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master')
                      Publisher = '*Christian Parpart*'
                      Secrets = @('ESIGNER_USERNAME', 'ESIGNER_PASSWORD', 'ESIGNER_CREDENTIAL_ID', 'ESIGNER_TOTP_SECRET')
                      PayloadStep = @{ Tool = 'codesigntool'; Invocation = 'Directory'; Timestamp = 'Tool'
                                       Arguments = @('-jar', '{toolDir:codesigntool}/jar/code_sign_tool-1.3.2.jar', 'batch_sign',
                                                     '-username={env:ESIGNER_USERNAME}', '-password={env:ESIGNER_PASSWORD}',
                                                     '-credential_id={env:ESIGNER_CREDENTIAL_ID}', '-totp_secret={env:ESIGNER_TOTP_SECRET}',
                                                     '-input_dir_path={inputDir}', '-output_dir_path={outputDir}') }
                      PackageStep = @{ Tool = 'codesigntool'; Invocation = 'Directory'; Timestamp = 'Tool'
                                       Arguments = @('-jar', '{toolDir:codesigntool}/jar/code_sign_tool-1.3.2.jar', 'batch_sign',
                                                     '-username={env:ESIGNER_USERNAME}', '-password={env:ESIGNER_PASSWORD}',
                                                     '-credential_id={env:ESIGNER_CREDENTIAL_ID}', '-totp_secret={env:ESIGNER_TOTP_SECRET}',
                                                     '-program_name={product:name}',
                                                     '-input_dir_path={inputDir}', '-output_dir_path={outputDir}') } }
        # A certificate the Windows store reaches: local signing, and the SimplySign Desktop fallback for Certum.
        # No AllowedBuilds, so CI cannot select it.
        signtool = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @(); Publisher = ''
                      Secrets = @('WINDOWS_SIGNING_CERT_SHA1')
                      PayloadStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://time.certum.pl', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/sha1', '{env:WINDOWS_SIGNING_CERT_SHA1}', '{file}') }
                      PackageStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://time.certum.pl', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/sha1', '{env:WINDOWS_SIGNING_CERT_SHA1}',
                                                     '/d', '{product:name}', '/du', '{product:url}', '{file}') } }
        # The throwaway-certificate pass: signs with a self-signed certificate made for one run, whose key lives in a
        # named container of the Microsoft Software Key Storage Provider and is deleted afterwards (a .pfx would leave
        # a key file in the user's profile for every signtool call). Not selectable by CI either.
        dryrun   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @(); Publisher = ''
                      Secrets = @('CONTOUR_SIGNING_CERT', 'CONTOUR_SIGNING_KEY')
                      PayloadStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://timestamp.digicert.com', 'http://timestamp.sectigo.com', 'http://time.certum.pl')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/f', '{env:CONTOUR_SIGNING_CERT}', '/csp', 'Microsoft Software Key Storage Provider',
                                                     '/kc', '{env:CONTOUR_SIGNING_KEY}', '{file}') }
                      PackageStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://timestamp.digicert.com', 'http://timestamp.sectigo.com', 'http://time.certum.pl')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/f', '{env:CONTOUR_SIGNING_CERT}', '/csp', 'Microsoft Software Key Storage Provider',
                                                     '/kc', '{env:CONTOUR_SIGNING_KEY}',
                                                     '/d', '{product:name}', '/du', '{product:url}', '{file}') } }
    }
}
