// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "topdownwidget.h"

#include <QMenu>
#include <QSortFilterProxyModel>

#include "TopDownViewItemModel.h"

void TopDownWidget::SetTopDownView(std::unique_ptr<TopDownView> top_down_view) {
  auto* model =
      new TopDownViewItemModel{std::move(top_down_view), ui_->topDownTreeView};
  auto* proxy_model = new QSortFilterProxyModel{ui_->topDownTreeView};
  proxy_model->setSourceModel(model);
  proxy_model->setSortRole(Qt::EditRole);
  ui_->topDownTreeView->setModel(proxy_model);
  ui_->topDownTreeView->sortByColumn(TopDownViewItemModel::kInclusive,
                                     Qt::DescendingOrder);
  ui_->topDownTreeView->header()->resizeSections(QHeaderView::ResizeToContents);
}

const QString TopDownWidget::kActionExpandRecursively =
    QStringLiteral("&Expand recursively");
const QString TopDownWidget::kActionCollapseRecursively =
    QStringLiteral("&Collapse recursively");
const QString TopDownWidget::kActionCollapseChildrenRecursively =
    QStringLiteral("Collapse children recursively");
const QString TopDownWidget::kActionExpandAll = QStringLiteral("Expand all");
const QString TopDownWidget::kActionCollapseAll =
    QStringLiteral("Collapse all");

static void ExpandRecursively(QTreeView* tree_view, const QModelIndex& index) {
  if (!index.isValid()) {
    return;
  }
  for (int i = 0; i < index.model()->rowCount(index); ++i) {
    const QModelIndex& child = index.child(i, 0);
    ExpandRecursively(tree_view, child);
  }
  if (!tree_view->isExpanded(index)) {
    tree_view->expand(index);
  }
}

static void CollapseRecursively(QTreeView* tree_view,
                                const QModelIndex& index) {
  if (!index.isValid()) {
    return;
  }
  for (int i = 0; i < index.model()->rowCount(index); ++i) {
    const QModelIndex& child = index.child(i, 0);
    CollapseRecursively(tree_view, child);
  }
  if (tree_view->isExpanded(index)) {
    tree_view->collapse(index);
  }
}

static void CollapseChildrenRecursively(QTreeView* tree_view,
                                        const QModelIndex& index) {
  if (!index.isValid()) {
    return;
  }
  for (int i = 0; i < index.model()->rowCount(index); ++i) {
    const QModelIndex& child = index.child(i, 0);
    CollapseRecursively(tree_view, child);
  }
}

void TopDownWidget::onCustomContextMenuRequested(const QPoint& point) {
  QModelIndex index = ui_->topDownTreeView->indexAt(point);
  if (!index.isValid()) {
    return;
  }

  std::vector<QModelIndex> selected_tree_indices;
  for (const QModelIndex& selected_index :
       ui_->topDownTreeView->selectionModel()->selectedIndexes()) {
    if (selected_index.column() != TopDownViewItemModel::kThreadOrFunction) {
      continue;
    }
    selected_tree_indices.push_back(selected_index);
  }

  bool enable_expand_recursively = false;
  bool enable_collapse_recursively = false;
  for (const QModelIndex& selected_index : selected_tree_indices) {
    if (selected_index.model()->rowCount(selected_index) > 0) {
      // As long as at least one of the selected nodes has children, always show
      // "Expand recursively", as even if the selected node is expanded there
      // could be subtrees not expanded. But only show "Collapse recursively"
      // and "Collapse children recursively" when at least one selected node is
      // expanded, as it would otherwise be unintuitive to collapse subtrees
      // none of which is visible.
      enable_expand_recursively = true;
      if (ui_->topDownTreeView->isExpanded(selected_index)) {
        enable_collapse_recursively = true;
      }
    }
  }

  QMenu menu{ui_->topDownTreeView};
  if (enable_expand_recursively) {
    menu.addAction(kActionExpandRecursively);
  }
  if (enable_collapse_recursively) {
    menu.addAction(kActionCollapseRecursively);
    menu.addAction(kActionCollapseChildrenRecursively);
  }
  menu.addSeparator();
  menu.addAction(kActionExpandAll);
  menu.addAction(kActionCollapseAll);

  QAction* action = menu.exec(ui_->topDownTreeView->mapToGlobal(point));
  if (action == nullptr) {
    return;
  }

  if (action->text() == kActionExpandRecursively) {
    for (const QModelIndex& selected_index : selected_tree_indices) {
      ExpandRecursively(ui_->topDownTreeView, selected_index);
    }
  } else if (action->text() == kActionCollapseRecursively) {
    for (const QModelIndex& selected_index : selected_tree_indices) {
      CollapseRecursively(ui_->topDownTreeView, selected_index);
    }
  } else if (action->text() == kActionCollapseChildrenRecursively) {
    for (const QModelIndex& selected_index : selected_tree_indices) {
      CollapseChildrenRecursively(ui_->topDownTreeView, selected_index);
    }
  } else if (action->text() == kActionExpandAll) {
    ui_->topDownTreeView->expandAll();
  } else if (action->text() == kActionCollapseAll) {
    ui_->topDownTreeView->collapseAll();
  }
}
