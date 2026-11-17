#include "SplitPageUndoAction.h"

#include <algorithm>  // for max
#include <memory>     // for make_shared, make_unique
#include <utility>    // for move

#include "control/Control.h"                         // for Control
#include "control/PageBackgroundChangeController.h"  // for PageBackgroundChangeController
#include "gui/MainWindow.h"                          // for MainWindow
#include "gui/XournalView.h"                         // for XournalView
#include "model/Document.h"                          // for Document
#include "model/Layer.h"                             // for Layer
#include "model/XojPage.h"                           // for XojPage
#include "util/i18n.h"                               // for _

#include "UndoRedoHandler.h"  // for UndoRedoHandler

SplitPageUndoAction::SplitPageUndoAction(const PageRef& page, const PageRef& newPage, double y, double oldHeight):
        UndoAction("SplitPageUndoAction"), newPage(newPage), y(y), oldHeight(oldHeight) {
    this->page = page;
}

void SplitPageUndoAction::splitPage(Control* control, const PageRef& page, double y) {
    Document* doc = control->getDocument();
    doc->lock_shared();
    const double sheet = page->isEndless() ? page->getSheetHeight() : page->getHeight();
    const double oldHeight = page->getHeight();
    auto newPage = std::make_shared<XojPage>(page->getWidth(), sheet, true);  // grows to fit what moves onto it
    newPage->setSheetHeight(page->getSheetHeight());
    PageBackgroundChangeController::copyBackgroundFromOtherPage(newPage, page);
    for (const Layer* layer: page->getLayersView()) {
        auto* l = new Layer();
        if (layer->hasName()) {
            l->setName(layer->getName());
        }
        l->setVisible(layer->isVisible());
        newPage->addLayer(l);
    }
    newPage->setSelectedLayerId(page->getSelectedLayerId());
    doc->unlock_shared();

    std::unique_ptr<SplitPageUndoAction> action(new SplitPageUndoAction(page, newPage, y, oldHeight));
    action->split(control);
    control->getUndoRedoHandler()->addUndoAction(std::move(action));
}

void SplitPageUndoAction::split(Control* control) {
    Document* doc = control->getDocument();
    doc->lock();
    const auto& layers = this->page->getLayers();
    const auto& newLayers = this->newPage->getLayers();
    this->movedPositions.assign(layers.size(), {});
    for (size_t li = 0; li < layers.size() && li < newLayers.size(); li++) {
        Layer* layer = layers[li];
        std::vector<const Element*> moving;
        Element::Index index = 0;
        for (const Element* e: layer->getElementsView()) {
            if (e->getY() + e->getElementHeight() / 2 >= this->y) {
                moving.push_back(e);
                this->movedPositions[li].push_back(index);
            }
            index++;
        }
        // remove from the back, so that the positions of the remaining ones stay right
        std::vector<ElementPtr> taken(moving.size());
        for (size_t i = moving.size(); i-- > 0;) {
            auto removed = layer->removeElementAt(moving[i], this->movedPositions[li][i]);
            taken[i] = std::move(removed.e);
        }
        for (auto& e: taken) {
            if (e) {
                e->move(0, -this->y);
                newLayers[li]->addElement(std::move(e));
            }
        }
    }
    this->page->setSize(this->page->getWidth(), this->y);
    const size_t pageNr = doc->indexOf(this->page);
    doc->insertPage(this->newPage, pageNr + 1);
    doc->unlock();

    control->firePageSizeChanged(pageNr);
    control->firePageInserted(pageNr + 1);
    control->growEndlessPage(this->newPage);
    control->getWindow()->getXournal()->forceUpdatePagenumbers();
    control->updatePageActions();
    this->undone = false;
}

void SplitPageUndoAction::join(Control* control) {
    Document* doc = control->getDocument();
    control->clearSelectionEndText();

    doc->lock_shared();
    const size_t newPageNr = doc->indexOf(this->newPage);
    doc->unlock_shared();
    if (newPageNr != npos) {
        // as for any page deletion: first tell the views, then delete the page
        control->firePageDeleted(newPageNr);
    }

    doc->lock();
    const auto& layers = this->page->getLayers();
    const auto& newLayers = this->newPage->getLayers();
    for (size_t li = 0; li < layers.size() && li < newLayers.size(); li++) {
        std::vector<ElementPtr> back = newLayers[li]->clearNoFree();
        const std::vector<Element::Index> noPositions;
        const auto& positions = li < this->movedPositions.size() ? this->movedPositions[li] : noPositions;
        for (size_t i = 0; i < back.size(); i++) {
            back[i]->move(0, this->y);
            if (i < positions.size()) {
                layers[li]->insertElement(std::move(back[i]), positions[i]);
            } else {
                layers[li]->addElement(std::move(back[i]));
            }
        }
    }
    if (newPageNr != npos) {
        doc->deletePage(newPageNr);
    }
    this->page->setSize(this->page->getWidth(), this->oldHeight);
    const size_t pageNr = doc->indexOf(this->page);
    doc->unlock();

    if (pageNr != npos) {
        control->firePageSizeChanged(pageNr);
    }
    control->getWindow()->getXournal()->forceUpdatePagenumbers();
    control->updatePageActions();
    this->undone = true;
}

auto SplitPageUndoAction::undo(Control* control) -> bool {
    join(control);
    return true;
}

auto SplitPageUndoAction::redo(Control* control) -> bool {
    split(control);
    return true;
}

auto SplitPageUndoAction::getText() -> std::string { return _("Start new page"); }

auto SplitPageUndoAction::getPages() -> std::vector<PageRef> { return {this->page, this->newPage}; }
