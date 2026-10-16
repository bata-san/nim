# Entity preparation survey

Run `nimcli --install <Marathon directory> survey-entities` to enumerate the same type-8
`8080881C` dynamic-model tags shown in the GUI's ENTITIES tab and prepare each through
`prepare_dynamic_model`, using `index_dynamic_model_materials` once as the GUI worker does.

Surveyed against the read-only install at `D:\SteamLibrary\steamapps\common\Marathon`:

| Result | Baseline | After fixes |
| --- | ---: | ---: |
| Entities | 2,366 | 2,366 |
| Drawable G-buffer models | 1,913 | 1,970 |
| No G-buffer parts (reported as no drawable) | 0 | 396 |
| Preparation failures | 453 | 0 |
| Indexed material components | 355 | 455 |

The baseline's 396 no-G-buffer models were thrown as failures with “dynamic model has no
drawable G-buffer parts”; they have parts assigned only to other render stages. These are now
reported in the GUI as no drawable geometry instead of as preparation errors. The other 57
failures had valid material maps whose technique arrays exceeded an arbitrary 4,096-entry cap.
Bounds checks against the component tag now validate the arrays without that cap.

The final survey still reports two partial TFX failures (`0x3C` at `0x0041`, in entities
`80B7A9C9` and `80B58B4C`). The opcode has no known semantics in the available reference VM, so
the affected material parts remain omitted rather than assigning guessed values. The rest of
each entity's drawable groups prepare successfully.
