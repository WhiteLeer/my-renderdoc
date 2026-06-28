/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "ThumbnailStrip.h"
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QIcon>
#include <QToolButton>
#include "Widgets/PanelCloseIcon.h"
#include "Widgets/ResourcePreview.h"
#include "ui_ThumbnailStrip.h"

ThumbnailStrip::ThumbnailStrip(QWidget *parent) : QWidget(parent), ui(new Ui::ThumbnailStrip)
{
  ui->setupUi(this);

  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet(QStringLiteral(
      "ThumbnailStrip { background: #1b232e; border: 1px solid #3a4758;"
      " border-radius: 8px; }"
      "#thumbnailPanelHeader { background: #202a36; border: none;"
      " border-top-left-radius: 7px; border-top-right-radius: 7px; }"
      "#thumbnailPanelTitle { color: #e7eef7; background: transparent;"
      " font-size: 11px; font-weight: 600; }"
      "#thumbnailPanelClose { color: #e7eef7; background: transparent; border: none;"
      " border-radius: 13px; padding: 0px; }"
      "#thumbnailPanelClose:hover { background: #354a62; }"
      "#thumbnailPanelClose:pressed { background: #4777a5; }"
      "QScrollArea { background: transparent; border: none; }"
      "QScrollArea QWidget#scrollAreaWidgetContents { background: transparent; }"
      "QScrollBar:vertical, QScrollBar:horizontal { background: #202a36; border: none; }"
      "QScrollBar::handle:vertical, QScrollBar::handle:horizontal {"
      " background: #43566c; border-radius: 3px; min-height: 18px; min-width: 18px; }"
      "QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }"));

  QGridLayout *grid = qobject_cast<QGridLayout *>(QWidget::layout());
  if(grid)
  {
    grid->removeWidget(ui->scrollArea);

    QFrame *header = new QFrame(this);
    header->setObjectName(QStringLiteral("thumbnailPanelHeader"));
    header->setFixedHeight(30);

    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 0, 5, 0);
    headerLayout->setSpacing(4);

    m_TitleLabel = new QLabel(windowTitle(), header);
    m_TitleLabel->setObjectName(QStringLiteral("thumbnailPanelTitle"));
    m_TitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerLayout->addWidget(m_TitleLabel);

    m_CloseButton = new QToolButton(header);
    m_CloseButton->setObjectName(QStringLiteral("thumbnailPanelClose"));
    m_CloseButton->setIcon(QIcon(QPixmap::fromImage(PanelCloseIcon::image())));
    m_CloseButton->setIconSize(QSize(19, 19));
    m_CloseButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_CloseButton->setFixedSize(28, 28);
    m_CloseButton->setToolTip(tr("Hide %1").arg(windowTitle()));
    headerLayout->addWidget(m_CloseButton);

    grid->addWidget(header, 0, 0);
    grid->addWidget(ui->scrollArea, 1, 0);

    QObject::connect(this, &QWidget::windowTitleChanged, this, [this](const QString &title) {
      if(m_TitleLabel)
        m_TitleLabel->setText(title);
      if(m_CloseButton)
        m_CloseButton->setToolTip(tr("Hide %1").arg(title));
    });
    QObject::connect(m_CloseButton, &QToolButton::clicked, this,
                     &ThumbnailStrip::closeRequested);
  }

  layout = new QVBoxLayout(ui->scrollAreaWidgetContents);
  layout->setSpacing(6);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setAlignment(Qt::AlignTop);
}

void ThumbnailStrip::setPanelTitle(const QString &title)
{
  setWindowTitle(title);
}

ThumbnailStrip::~ThumbnailStrip()
{
  delete layout;
  delete ui;
}

void ThumbnailStrip::addThumb(ResourcePreview *prev)
{
  layout->addWidget(prev);
  m_Thumbnails.push_back(prev);
}

void ThumbnailStrip::clearThumbs()
{
  for(ResourcePreview *p : m_Thumbnails)
  {
    layout->removeWidget(p);
    delete p;
  }

  m_Thumbnails.clear();
}

void ThumbnailStrip::resizeEvent(QResizeEvent *event)
{
  refreshLayout();
}

void ThumbnailStrip::mousePressEvent(QMouseEvent *event)
{
  emit(mouseClick(event));
}

void ThumbnailStrip::showEvent(QShowEvent *event)
{
  refreshLayout();
}

void ThumbnailStrip::refreshLayout()
{
  if(updatesEnabled())
    setUpdatesEnabled(false);

  QRect avail = geometry();
  avail.adjust(6, 6, -6, -6);

  int numActive = 0;
  for(ResourcePreview *c : m_Thumbnails)
    if(c->isActive())
      numActive++;

  // Inputs and Outputs are intentionally fixed to a vertical list. The old adaptive
  // branch changed orientation when the dock area crossed its aspect-ratio threshold,
  // which made resizing the main window unexpectedly change the interaction layout.
  {
    avail.setHeight(avail.height() + 6);    // controls implicitly have a 6 margin on the bottom

    int aspectHeight = (int)((float)avail.width() / 1.3f);

    ui->scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    delete layout;
    layout = new QVBoxLayout();
    layout->setSpacing(6);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setAlignment(Qt::AlignTop);

    int noscrollHeight = numActive * (aspectHeight + 6);

    if(noscrollHeight <= avail.height())
    {
      ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

      for(ResourcePreview *c : m_Thumbnails)
        c->setSize(QSize(avail.width(), aspectHeight));
    }
    else
    {
      ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

      QScrollBar *vs = ui->scrollArea->verticalScrollBar();

      avail.setWidth(qMax(1, avail.width() - vs->geometry().width()));

      aspectHeight = qMax(1, (int)((float)avail.width() / 1.3f));

      int totalHeight = numActive * (aspectHeight + 6);
      vs->setEnabled(totalHeight > avail.height());

      for(ResourcePreview *c : m_Thumbnails)
        c->setSize(QSize(avail.width(), aspectHeight));
    }
  }

  for(ResourcePreview *w : m_Thumbnails)
    layout->addWidget(w);

  for(ResourcePreview *w : m_Thumbnails)
    if(w->isActive())
      w->show();

  ui->scrollAreaWidgetContents->setLayout(layout);

  setUpdatesEnabled(true);
}
