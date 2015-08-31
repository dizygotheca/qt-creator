/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "diagnosticschangedmessage.h"

#include "container_common.h"

#include <QDataStream>
#include <QDebug>

namespace ClangBackEnd {

DiagnosticsChangedMessage::DiagnosticsChangedMessage(const FileContainer &file,
                                                     const QVector<DiagnosticContainer> &diagnostics,
                                                     quint32 documentRevision)
    : file_(file),
      diagnostics_(diagnostics),
      documentRevision_(documentRevision)
{
}

const FileContainer &DiagnosticsChangedMessage::file() const
{
    return file_;
}

const QVector<DiagnosticContainer> &DiagnosticsChangedMessage::diagnostics() const
{
    return diagnostics_;
}

quint32 DiagnosticsChangedMessage::documentRevision() const
{
    return documentRevision_;
}

QDataStream &operator<<(QDataStream &out, const DiagnosticsChangedMessage &message)
{
    out << message.file_;
    out << message.diagnostics_;
    out << message.documentRevision_;

    return out;
}

QDataStream &operator>>(QDataStream &in, DiagnosticsChangedMessage &message)
{
    in >> message.file_;
    in >> message.diagnostics_;
    in >> message.documentRevision_;

    return in;
}

bool operator==(const DiagnosticsChangedMessage &first, const DiagnosticsChangedMessage &second)
{
    return first.file_ == second.file_
            && first.diagnostics_ == second.diagnostics_;
}

bool operator<(const DiagnosticsChangedMessage &first, const DiagnosticsChangedMessage &second)
{
    return first.file_ < second.file_
            && compareContainer(first.diagnostics_, second.diagnostics_);
}

QDebug operator<<(QDebug debug, const DiagnosticsChangedMessage &message)
{
    debug.nospace() << "DiagnosticsChangedMessage("
                    << message.file_ << QStringLiteral(", ")
                    << message.documentRevision_
                    << ")";

    return debug;
}

void PrintTo(const DiagnosticsChangedMessage &message, ::std::ostream* os)
{
    *os << "DiagnosticsChangedMessage(";
    PrintTo(message.file(), os);
    *os << ")";
}

} // namespace ClangBackEnd
