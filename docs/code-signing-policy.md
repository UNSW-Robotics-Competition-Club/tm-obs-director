# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/),
certificate by [SignPath Foundation](https://signpath.org/).

## What is signed

Only tagged releases built by this repository's GitHub Actions workflow, on
GitHub-hosted runners, from the public source in this repository. Each release
signs:

- `tm-obs-director.dll`, the plugin itself, before it is packed into the zip or
  the installer
- `tm-obs-director-<version>-windows-x64-installer.exe`

Nothing built on a developer's machine is signed, and nothing from another
project is signed with this certificate.

The build bakes in a Cerberus client key from a repository secret (see
[Building](../README.md#building)). That key is the only input to a release that
is not in this repository.

## Team roles

<!-- TODO: fill in GitHub usernames before applying to SignPath Foundation. -->

- **Committers and reviewers:** members of the
  [UNSW Robotics Competition Club](https://github.com/UNSW-Robotics-Competition-Club)
  GitHub organisation with write access to this repository
- **Approvers:** TODO

Approvers accept each signing request by hand in SignPath before it is
signed. Everyone in these roles
uses multi-factor authentication on both GitHub and SignPath.

## Privacy

This program does not send information anywhere unless the person operating it
sets it up to. It connects to:

- the Tournament Manager server whose address the operator enters, to read
  field and match state
- `tm.unswrobotics.com`, to get the sign-in token Tournament Manager requires.
  That request carries the plugin's build key and version and, where set, the
  event's code, name and SKU. It carries nothing about the people at the event.

Settings, including the scene mapping, stay in the operator's OBS profile on
their own computer.
