# The Windows code-signing signers, as data. scripts/windows-signing/WindowsSigning.psm1 interprets this table and
# Test-SigningTable checks it; adding a signer or a tool is a new row here, not a change of logic.
#
# A signer signs the executables of the payload (what the installers carry) and then the packages themselves.
#   Transport      None: nothing is signed. SignPath: the SignPath action signs and hands back. Command: a tool is run
#                  per file (the steps below).
#   Payload        Own: only OwnBinaries are signed (a signer that may sign the project's own binaries only).
#                  Unsigned: every portable executable that is not signed already.
#   AllowedBuilds  Which builds a signer may sign: releases (a release tag), releases-and-master (master pushes too).
#                  Empty: never selectable by CI, only run by hand.
#   Invocation     InPlace: the tool signs the file it is given. InOut: it reads {in} and writes {out}. Directory: it
#                  signs the files of {inputDir} into {outputDir}.
#
# Arguments take {file}, {in}, {out}, {inputDir}, {outputDir}, {timestamp}, {env:NAME} (a secret, never printed),
# {tool:NAME} (a tool's path) and {data:FILE} (a file of scripts/windows-signing/data).
@{
    # What a signer limited to the project's own binaries signs.
    OwnBinaries = @('bin/contour.exe')
    # Per invocation; attempt k uses TimestampUrls[k % n], so a timestamp server that is down is not asked twice.
    Attempts = 3
    RetryDelaySeconds = @(5, 15)
    # The tools, each pinned: a Windows SDK or PATH program, or a release asset of a repository by tag and SHA-256.
    Tools = @{
        signtool     = @{ Source = 'WindowsSdk'; Executable = 'signtool.exe' }
        ssign        = @{ Source = 'GitHubRelease'; Repository = 'Le-Syl21/ssign'; Tag = 'v0.1.10'
                          Asset = 'ssign-windows-x86_64.zip'
                          Sha256 = '68acfc7535830abb716c85a08f28d36dd46ed3098e3cf5c6f0ebe3c8c6694516'; Executable = 'ssign.exe' }
        ssignPkcs11  = @{ Source = 'GitHubRelease'; Repository = 'Le-Syl21/ssign'; Tag = 'v0.1.10'
                          Asset = 'ssign-pkcs11-windows-x86_64.zip'
                          Sha256 = 'c546a565f174b265c1ab3359fed8e7afb243adacb8113296791d77ac60cac0b6'; Executable = 'ssign_pkcs11.dll' }
        osslsigncode = @{ Source = 'GitHubRelease'; Repository = 'mtrojnar/osslsigncode'; Tag = '2.14'
                          Asset = 'osslsigncode-2.14-windows-x64-mingw.zip'
                          Sha256 = '9a1722aaf62a27852c4eb9c35749a0248065052d0ae0a93d4ed6bb49def027f2'; Executable = 'bin/osslsigncode.exe' }
        libp11       = @{ Source = 'GitHubRelease'; Repository = 'OpenSC/libp11'; Tag = 'libp11-0.4.21'
                          Asset = 'libp11-0.4.21-x64.zip'
                          Sha256 = '443f6ddbf15b437df0e37190c8673e76fa5f7268d22d99052d1e9b2fe1af46fd'; Executable = 'pkcs11prov.dll' }
        # Both values are pinned once the archive has been read (the task that adds the signer).
        codesigntool = @{ Source = 'GitHubRelease'; Repository = 'SSLcom/CodeSignTool'; Tag = 'v1.3.2'
                          Asset = 'CodeSignTool-v1.3.2-windows.zip'
                          Sha256 = '<pinned in Task 4>'; Executable = '<path inside the archive, Task 4>' }
    }
    Signers = @{
        none     = @{ Transport = 'None'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master') }
        signpath = @{ Transport = 'SignPath'; Payload = 'Own'; AllowedBuilds = @('releases') }
        certum   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master')
                      Secrets = @('CERTUM_EMAIL', 'CERTUM_OTP')
                      PayloadStep = @{ Tool = 'ssign'; Invocation = 'InPlace'; Arguments = @('{file}') }
                      PackageStep = @{ Tool = 'osslsigncode'; Invocation = 'InOut'; TimestampUrls = @('http://time.certum.pl/')
                                       Arguments = @('sign', '-provider', '{tool:libp11}', '-pkcs11module', '{tool:ssignPkcs11}',
                                                     '-pkcs11cert', 'pkcs11:type=cert', '-key', 'pkcs11:type=private',
                                                     '-ac', '{data:certum-code-signing-2021-ca.pem}', '-h', 'sha256',
                                                     '-n', 'Contour', '-i', 'https://github.com/contour-terminal/contour/',
                                                     '-ts', '{timestamp}', '-in', '{in}', '-out', '{out}') } }
        # CodeSignTool's batch_sign signs a directory into another. Whether it takes -program_name is read from its
        # help when the signer is added; until then the package step is the payload step.
        sslcom   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @('releases', 'releases-and-master')
                      Secrets = @('ESIGNER_USERNAME', 'ESIGNER_PASSWORD', 'ESIGNER_CREDENTIAL_ID', 'ESIGNER_TOTP_SECRET')
                      PayloadStep = @{ Tool = 'codesigntool'; Invocation = 'Directory'
                                       Arguments = @('batch_sign', '-username={env:ESIGNER_USERNAME}', '-password={env:ESIGNER_PASSWORD}',
                                                     '-credential_id={env:ESIGNER_CREDENTIAL_ID}', '-totp_secret={env:ESIGNER_TOTP_SECRET}',
                                                     '-input_dir_path={inputDir}', '-output_dir_path={outputDir}') }
                      PackageStep = @{ Tool = 'codesigntool'; Invocation = 'Directory'
                                       Arguments = @('batch_sign', '-username={env:ESIGNER_USERNAME}', '-password={env:ESIGNER_PASSWORD}',
                                                     '-credential_id={env:ESIGNER_CREDENTIAL_ID}', '-totp_secret={env:ESIGNER_TOTP_SECRET}',
                                                     '-input_dir_path={inputDir}', '-output_dir_path={outputDir}') } }
        # A certificate the Windows store reaches: local signing, and the SimplySign Desktop fallback for Certum.
        # No AllowedBuilds, so CI cannot select it.
        signtool = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @()
                      Secrets = @('WINDOWS_SIGNING_CERT_SHA1')
                      PayloadStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://time.certum.pl', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/sha1', '{env:WINDOWS_SIGNING_CERT_SHA1}', '{file}') }
                      PackageStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://time.certum.pl', 'http://timestamp.digicert.com', 'http://timestamp.sectigo.com')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/sha1', '{env:WINDOWS_SIGNING_CERT_SHA1}',
                                                     '/d', 'Contour', '/du', 'https://github.com/contour-terminal/contour/', '{file}') } }
        # The throwaway-certificate pass: signs with a self-signed .pfx made for one run, to see the whole path
        # work without a vendor. Not selectable by CI either.
        dryrun   = @{ Transport = 'Command'; Payload = 'Unsigned'; AllowedBuilds = @()
                      Secrets = @('CONTOUR_SIGNING_PFX', 'CONTOUR_SIGNING_PFX_PASSWORD')
                      PayloadStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://timestamp.digicert.com', 'http://timestamp.sectigo.com', 'http://time.certum.pl')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/f', '{env:CONTOUR_SIGNING_PFX}', '/p', '{env:CONTOUR_SIGNING_PFX_PASSWORD}', '{file}') }
                      PackageStep = @{ Tool = 'signtool'; Invocation = 'InPlace'
                                       TimestampUrls = @('http://timestamp.digicert.com', 'http://timestamp.sectigo.com', 'http://time.certum.pl')
                                       Arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', '{timestamp}',
                                                     '/f', '{env:CONTOUR_SIGNING_PFX}', '/p', '{env:CONTOUR_SIGNING_PFX_PASSWORD}',
                                                     '/d', 'Contour', '/du', 'https://github.com/contour-terminal/contour/', '{file}') } }
    }
}
