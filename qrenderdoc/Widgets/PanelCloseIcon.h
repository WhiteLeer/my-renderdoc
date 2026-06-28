#ifndef RENDERDOC_PANEL_CLOSE_ICON_H
#define RENDERDOC_PANEL_CLOSE_ICON_H

#include <QImage>
#include <QPainter>

namespace PanelCloseIcon
{
inline QImage image()
{
  QImage source(QSize(32, 32), QImage::Format_ARGB32_Premultiplied);
  source.fill(Qt::transparent);

  QPainter painter(&source);
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(QColor(QStringLiteral("#e7eef7")));
  pen.setWidthF(2.6);
  pen.setCapStyle(Qt::RoundCap);
  painter.setPen(pen);
  painter.drawLine(QPointF(8.0, 8.0), QPointF(24.0, 24.0));
  painter.drawLine(QPointF(24.0, 8.0), QPointF(8.0, 24.0));

  return source.scaled(QSize(19, 19), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
}

#endif
