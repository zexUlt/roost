import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The links of one panel (design.md §6): for each input of the panel, which
// output feeds it, or none. A list, not a graph: an input offers only the
// outputs of the other panels whose declared type is the type of the input.
// The dialog changes nothing itself: a choice is `linkChosen`, the shell
// applies it and gives the dialog the new state with `show`.
Dialog {
    id: root

    // {instance, panel, inputs: [{input, type, from, output, problem,
    // choices: [{from, output, label}]}]}, as the shell gives it; null before
    // the first `show`.
    property var model: null

    // The user chose `from`.`output` for `input` of `instance`; `from` is empty
    // for "none".
    signal linkChosen(string instance, string input, string from, string output)

    // The input whose box had the keyboard last.
    property string focusedInput: ""

    onClosed: root.focusedInput = ""

    // Shows the state of the panel; opens the dialog if it is not open. A new
    // state of an open dialog makes the boxes anew, and the one that had the
    // keyboard is deleted with it: the keyboard goes to the new box of the
    // input, else to the dialog, or Escape would not reach it.
    function show(state) {
        root.model = state;
        if (!root.visible) {
            root.open();
            return;
        }
        let target = root.contentItem;
        for (let i = 0; i < inputs.count; ++i) {
            const row = inputs.itemAt(i);
            if (row !== null && row.modelData.input === root.focusedInput) {
                target = row.box;
            }
        }
        target.forceActiveFocus();
    }

    // The entries of the box of an input: none, the link of the document if it
    // is not one of the choices (it does not work), then the choices.
    function optionsOf(input) {
        const options = [
            {
                label: "none",
                from: "",
                output: ""
            }
        ];
        const known = input.choices.some(choice => choice.from === input.from && choice.output === input.output);
        if (input.from !== "" && !known) {
            options.push({
                label: input.from + "." + input.output + " (off" + (input.problem !== "" ? ": " + input.problem : "") + ")",
                from: input.from,
                output: input.output
            });
        }
        return options.concat(input.choices);
    }

    function indexOfCurrent(options, input) {
        const found = options.findIndex(option => option.from === input.from && option.output === input.output);
        return found < 0 ? 0 : found;
    }

    objectName: "linkEditor"
    title: root.model !== null ? "Links of " + root.model.instance + " (" + root.model.panel + ")" : "Links"
    modal: true
    width: 480
    anchors.centerIn: Overlay.overlay
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.Close
    padding: Roost.theme.panelPadding

    background: Rectangle {
        radius: Roost.theme.radiusCard
        color: Roost.theme.surface
        border.width: 1
        border.color: Roost.theme.line
    }

    header: Label {
        text: root.title
        font: Roost.theme.headingFont
        elide: Text.ElideRight
        padding: Roost.theme.panelPadding
        bottomPadding: 0
    }

    footer: DialogButtonBox {
        padding: Roost.theme.panelPadding
        topPadding: 0
        spacing: 8
        background: null
        delegate: FormButton {}
    }

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            objectName: "linkEditorEmpty"
            visible: root.model === null || root.model.inputs.length === 0
            text: "This panel has no inputs to link."
            color: Roost.theme.textMuted
        }

        Repeater {
            id: inputs

            model: root.model !== null ? root.model.inputs : []

            delegate: RowLayout {
                id: row

                required property var modelData
                property alias box: box

                readonly property var options: root.optionsOf(modelData)

                Layout.fillWidth: true
                spacing: 8

                Label {
                    objectName: "linkInputLabel"
                    Layout.preferredWidth: 140
                    text: row.modelData.input + " (" + row.modelData.type + ")"
                    elide: Text.ElideRight
                }

                FormComboBox {
                    id: box

                    objectName: "linkInput_" + row.modelData.input
                    Layout.fillWidth: true
                    model: row.options
                    textRole: "label"
                    currentIndex: root.indexOfCurrent(row.options, row.modelData)
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.focusedInput = row.modelData.input;
                        }
                    }
                    onActivated: index => root.linkChosen(root.model.instance, row.modelData.input, row.options[index].from, row.options[index].output)
                }
            }
        }
    }
}
