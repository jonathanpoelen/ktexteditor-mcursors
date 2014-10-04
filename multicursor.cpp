 /*
* This file is part of Katepart
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "multicursor.h"
#include "multicursorconfig.h"

#include <functional>
#include <algorithm>

#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KTextEditor/MovingInterface>

#include <KPluginFactory>
#include <KPluginLoader>
#include <KLocale>
#include <KAction>
#include <KActionCollection>
#include <KActionMenu>

#include <QMenu>
#include <QApplication>
#include <KConfigGroup>

MultiCursorPlugin *MultiCursorPlugin::plugin = 0;

K_PLUGIN_FACTORY_DEFINITION(MultiCursorPluginFactory,
	registerPlugin<MultiCursorPlugin>("ktexteditor_multicursor");
	registerPlugin<MultiCursorConfig>("ktexteditor_multicursor_config");
)
K_EXPORT_PLUGIN(MultiCursorPluginFactory("ktexteditor_multicursor", "ktexteditor_plugins"))

MultiCursorPlugin::MultiCursorPlugin(QObject *parent, const QVariantList &args)
: KTextEditor::Plugin(parent)
, m_views()
, m_attr(new KTextEditor::Attribute)
, m_remove_cursor_if_only_click(false)
, m_active_ctrl_click(false)
{
	Q_UNUSED(args);
	plugin = this;

	readConfig();
}

MultiCursorPlugin::~MultiCursorPlugin()
{
	plugin = 0;
}

void MultiCursorPlugin::addView(KTextEditor::View *view)
{
    MultiCursorView *nview = new MultiCursorView(view, m_attr);
    if (m_active_ctrl_click) {
        nview->setActiveCtrlClick(true, m_remove_cursor_if_only_click);
    }
    m_views.append(nview);
}

void MultiCursorPlugin::removeView(KTextEditor::View *view)
{
	for (int z = 0; z < m_views.size(); z++) {
		if (m_views.at(z)->parentClient() == view) {
			MultiCursorView *nview = m_views.at(z);
			m_views.removeAll(nview);
			delete nview;
		}
    }
    if (m_active_ctrl_click && m_views.empty()) {
        QApplication::instance()->removeEventFilter(this);
    }
}

void MultiCursorPlugin::readConfig()
{
	KConfigGroup cg(KGlobal::config(), "MultiCursor Plugin");
	const DefaultValues values;
	m_attr->setBackground(cg.readEntry("cursor_brush", values.cursorColor));
	m_attr->setUnderlineColor(cg.readEntry("underline_color", values.underlineColor));
	int line_style = cg.readEntry("underline_style", values.underlineStyle);
	m_attr->setUnderlineStyle(QTextCharFormat::UnderlineStyle(line_style));
    m_remove_cursor_if_only_click = cg.readEntry("remove_cursor_if_only_click", false);
	m_active_ctrl_click = cg.readEntry("active_ctrl_click", true);
}

void MultiCursorPlugin::writeConfig()
{
	KConfigGroup cg(KGlobal::config(), "MultiCursor Plugin");
	cg.writeEntry("cursor_brush", m_attr->background().color());
	cg.writeEntry("underline_color", m_attr->underlineColor());
    cg.writeEntry("underline_style", int(m_attr->underlineStyle()));
    cg.writeEntry("remove_cursor_if_only_click", m_remove_cursor_if_only_click);
	cg.writeEntry("active_ctrl_click", m_active_ctrl_click);
}

void MultiCursorPlugin::setActiveCtrlClick(bool active, bool remove_cursor_if_only_click)
{
    m_active_ctrl_click = active;
    m_remove_cursor_if_only_click = remove_cursor_if_only_click;
    for (MultiCursorView * v: m_views) {
        v->setActiveCtrlClick(active, remove_cursor_if_only_click);
    }
}


struct MultiCursorView::CursorListDetail
{
	template<typename Predicate>
	static CursorList::iterator
	move_line(KTextEditor::Document* doc, CursorList& cont, Predicate predicate,
						CursorList::iterator first, int line)
	{
		int pl = -1;
		int pc = -1;
		CursorList::iterator last = cont.end();
		while (first != last && predicate(*first)) {
			int l = first->line() + line;
			int c = std::min(first->column(), doc->lineLength(l));
			if (l == pl && c == pc) {
				first = cont.erase(first);
				continue ;
			}
			first->setCursor(l, c);
			pl = l;
			pc = c;
			++first;
		}
		return first;
	}

  static KTextEditor::Cursor
  recoil(
    KTextEditor::Document* doc,
    const KTextEditor::Cursor& cursor,
    int length, int minline = 0
  ) {
    if (cursor.column() >= length) {
      return KTextEditor::Cursor(cursor.line(), cursor.column() - length);
    }
    KTextEditor::Cursor ret(cursor.line() - 1, 0);
    length -= cursor.column();
    while (length && ret.line() >= minline) {
      const int line_length = doc->lineLength(ret.line());
      if (line_length < length) {
        ret.setLine(ret.line() - 1);
        length -= line_length - 1;
      }
      else {
        ret.setColumn(line_length - length + 1);
        length = 0;
      }
    }
    return ret;
  }

  static KTextEditor::Cursor
  advance(
    KTextEditor::Document* doc,
    const KTextEditor::Cursor& cursor,
    int length, int endline
  ) {
    KTextEditor::Cursor ret(cursor);
    int line_length = doc->lineLength(cursor.line());
    if (ret.column() + length - 1 < line_length)
    ret.setColumn(ret.column() + length);
    else
    {
      length -= line_length - ret.column() + 1;
      ret.setColumn(0);
      ret.setLine(ret.line() + 1);
      while (length && ret.line() < endline)
      {
        line_length = doc->lineLength(ret.line());
        if (line_length < length)
        {
          ret.setLine(ret.line() + 1);
          length -= line_length - 1;
        }
        else
        {
          ret.setColumn(length);
          length = 0;
        }
      }
    }
    return ret;
  }

  static void removeForwardText(
    KTextEditor::Document* doc, CursorList & cursors, int length)
  {
    CursorList::reverse_iterator last = cursors.rend();
    CursorList::reverse_iterator it = cursors.rbegin();
    KTextEditor::Cursor cursor
      = advance(doc, it->cursor(), length, doc->lines()+1);
    doc->removeText(KTextEditor::Range(it->cursor(), cursor));
    CursorList::reverse_iterator prev = it;
    while (++it != last) {
      cursor = advance(doc, it->cursor(), length, prev->cursor().line()+1);
      if (prev->cursor() <= cursor) {
        doc->removeText(KTextEditor::Range(it->cursor(), prev->cursor()));
        prev = CursorList::reverse_iterator(cursors.erase(it.base()));
        it = prev;
      } else {
        doc->removeText(KTextEditor::Range(it->cursor(), cursor));
        ++prev;
      }
    }
    for (Cursor& c: cursors) {
      c.revalid();
    }
  }

  template<class Pred>
  static void removeBackwardText(
    KTextEditor::Document* doc, CursorList & cursors, int length, Pred pred)
  {
    CursorList::iterator first = cursors.begin();
    CursorList::iterator last = cursors.end();
    if (pred(*first)) {
      doc->removeText(KTextEditor::Range(
        first->cursor(), recoil(doc, first->cursor(), length)
      ));
    }
    KTextEditor::Cursor cursor_prev = first->cursor();
    while (++first != last) {
      KTextEditor::Cursor cursor
        = recoil(doc, first->cursor(), length, cursor_prev.line());
      if (cursor_prev >= cursor) {
        if (pred(*first)) {
          doc->removeText(KTextEditor::Range(first->cursor(), cursor_prev));
        }
        first = cursors.erase(--first);
        last = cursors.end();
        cursor_prev = cursor;
      } else if (pred(*first)) {
        doc->removeText(KTextEditor::Range(first->cursor(), cursor));
      }
    }
  }
};

MultiCursorView::MultiCursorView(KTextEditor::View *view, KTextEditor::Attribute::Ptr attr)
: QObject(view)
, KXMLGUIClient(view)
, m_view(view)
, m_document(view->document())
, m_smart(qobject_cast<KTextEditor::MovingInterface*>(m_document))
, m_text_edit(false)
, m_active(true)
, m_synchronize(false)
, m_remove_cursor_if_only_click(false)
, m_cursor()
, m_attr(attr)
{
	setComponentData(MultiCursorPluginFactory::componentData());

	KActionCollection* collection = actionCollection();
	KAction *action;

#define ENTRY(Text, Name, Receiver) \
	action = new KAction(i18n(Text), this);\
	collection->addAction(Name, action);\
	connect(action, SIGNAL(triggered()), this, SLOT(Receiver));

	/*ENTRY("info cursors", "info_multicursor", debug());
	action->setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_I);*/

	ENTRY("Set MultiCursor", "set_multicursor", setCursor());
	action->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_C);

	ENTRY("Backspace Character for MultiCursor", "backspace_multicursor", textBackspace());
	action->setShortcut(Qt::ALT + Qt::Key_Backspace);

	ENTRY("Delete Character for MultiCursor", "del_multicursor", textDelete());
	action->setShortcut(Qt::ALT + Qt::Key_Delete);

	ENTRY("Remove all MultiCursor", "remove_all_multicursor", removeAll());
	action->setShortcut(Qt::ALT + Qt::SHIFT + Qt::Key_Delete);

	ENTRY("Remove line MultiCursor", "remove_line_multicursor", removeLine());
	action->setShortcut(Qt::CTRL + Qt::ALT +  Qt::Key_Delete);

	ENTRY("Move next MultiCursor", "move_next_multicursor", moveNext());
	action->setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_H);

	ENTRY("Move prev MultiCursor", "move_prev_multicursor", movePrev());
	action->setShortcut(Qt::CTRL + Qt::ALT + Qt::SHIFT + Qt::Key_H);

	ENTRY("Set active MultiCursor", "active_multicursor", setActive())
	action->setCheckable(true);
	action->setChecked(true);

	ENTRY("Synchronize with the current cursor", "synchronise_multicursor", setSynchronize());
	action->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_P);
	action->setCheckable(true);

#undef ENTRY

	setEnabled(false);
	setXMLFile("multicursorui.rc");
}

MultiCursorView::~MultiCursorView()
{}

void MultiCursorView::exclusiveEditStart(KTextEditor::Document *doc)
{
	Q_UNUSED(doc);
	m_text_edit = true;
}

void MultiCursorView::exclusiveEditEnd(KTextEditor::Document *doc)
{
	Q_UNUSED(doc);
	m_text_edit = false;
}

void MultiCursorView::textBackspace()
{
  if (startEditing()) {
    CursorListDetail::removeBackwardText(
      m_document, m_cursors, 1,
      [](Cursor const &) { return true; }
    );
    endEditing();
  }
}

void MultiCursorView::textDelete()
{
  if (startEditing()) {
    CursorListDetail::removeForwardText(m_document, m_cursors, 1);
    endEditing();
  }
}

#define MSIGNAL_OBJECT(O, F, P) F(O, SIGNAL(P), this, SLOT(P))
#define MSIGNAL(F, P) MSIGNAL_OBJECT(m_document, F, P)

#define MSIGNALCURSES(F)\
	do { \
		MSIGNAL(F, textRemoved(KTextEditor::Document*,KTextEditor::Range,QString));\
		MSIGNAL(F, textInserted(KTextEditor::Document*,KTextEditor::Range));\
		MSIGNAL(F, exclusiveEditStart(KTextEditor::Document*));\
		MSIGNAL(F, exclusiveEditEnd(KTextEditor::Document*));\
	} while (0)

#define MSIGNALCURSES_SYNCHRONISE(F)\
	MSIGNAL_OBJECT(m_view, F, cursorPositionChanged(KTextEditor::View*,KTextEditor::Cursor))

void MultiCursorView::connectCurses()
{
	MSIGNALCURSES(connect);
	if (m_synchronize) {
		MSIGNALCURSES_SYNCHRONISE(connect);
	}
}

void MultiCursorView::disconnectCurses()
{
	MSIGNALCURSES(disconnect);
	if (m_synchronize) {
		MSIGNALCURSES_SYNCHRONISE(disconnect);
	}
}

#undef MSIGNALCURSES

void MultiCursorView::setSynchronize()
{
	if (m_synchronize) {
		m_synchronize = false;
		MSIGNALCURSES_SYNCHRONISE(disconnect);
	} else {
		m_synchronize = true;
		m_cursor = m_view->cursorPosition();
		MSIGNALCURSES_SYNCHRONISE(connect);
	}
}

#undef MSIGNALCURSES_SYNCHRONISE
#undef MSIGNAL
#undef MSIGNAL_OBJECT

void MultiCursorView::actionEmptyCurses()
{
	disconnectCurses();
	setEnabled(false);
}

void MultiCursorView::actionStartCurses()
{
	connectCurses();
	setEnabled(true);
}

void MultiCursorView::setEnabled(bool x)
{
	KActionCollection * collec = actionCollection();
	collec->action("backspace_multicursor")->setEnabled(x);
	collec->action("remove_all_multicursor")->setEnabled(x);
	collec->action("remove_line_multicursor")->setEnabled(x);
	collec->action("move_next_multicursor")->setEnabled(x);
	collec->action("move_prev_multicursor")->setEnabled(x);
	collec->action("synchronise_multicursor")->setEnabled(x);
}

void MultiCursorView::cursorPositionChanged(KTextEditor::View*, const KTextEditor::Cursor& cursor)
{
	int l1 = m_cursor.line();
	int l2 = cursor.line();
	const int line = l2-l1;
	const int c1 = m_cursor.column();
	const int c2 = cursor.column();
	const int column = c2-c1;

	m_cursor = cursor;

	if (0 == line && 0 == column)
		return ;

	if (0 == column && line < 0) {
        auto pred = [](Cursor & c, int line) { return c.line() != line; };
        auto first = std::lower_bound(
          m_cursors.begin(), m_cursors.end(), -line, pred);
        first = m_cursors.erase(m_cursors.begin(), first);
        CursorListDetail::move_line(m_document, m_cursors, [](Cursor&){return true;}, first, line);
	}
	else if (0 == column && line > 0) {
		int last_line = m_document->lines();
		m_cursors.erase(
			CursorListDetail::move_line(
				m_document, m_cursors,
				[line, last_line](Cursor& cur){
					return cur.line() + line < last_line;
				},
				m_cursors.begin(),
				line
			),
			m_cursors.end()
		);
	}
	else if (line < 0 || (line == 0 && column < 0)) {
		int n = line ? c1 + m_document->lineLength(l2) - c2 + line + 2 : -column;
		while (++l2 < l1) {
			n += m_document->lineLength(l2);
		}
		CursorList::iterator first = m_cursors.begin();
		CursorList::iterator last = m_cursors.end();
		for (; first != last; ++first) {
			KTextEditor::Cursor cur
              = CursorListDetail::recoil(m_document, first->cursor(), n, -1);
			if (cur.isValid()) {
				first->setCursor(cur);
				break;
			}
		}
		first = m_cursors.erase(m_cursors.begin(), first);
        if (first != last) {
            while (++first != last) {
              first->setCursor(
                CursorListDetail::recoil(m_document, first->cursor(), n, -1)
              );
            }
        }
	}
	else {
		int n = line ? c2 + m_document->lineLength(l1) - (c1 + line) + 2 : column;
		while (++l1 < l2) {
			n += m_document->lineLength(l1);
		}
		CursorList::iterator first = m_cursors.begin();
		CursorList::iterator last = m_cursors.end();
		const int lines = m_document->lines() + 1;
		KTextEditor::Cursor docend = m_document->documentEnd();
		for (; first != last; ++first) {
			KTextEditor::Cursor cur
              = CursorListDetail::advance(m_document, first->cursor(), n, lines);
			if (cur > docend) {
				break;
			}
			first->setCursor(cur);
		}
		m_cursors.erase(first, last);
	}

	if (m_cursors.empty())
		actionEmptyCurses();
}

void MultiCursorView::setCursor(const KTextEditor::Cursor& cursor)
{
  auto it = std::lower_bound(
    m_cursors.begin(), m_cursors.end(), m_view->cursorPosition());
  if (it != m_cursors.end() && *it == cursor) {
    removeRange(it);
  }
  else {
    KTextEditor::MovingRange * range = m_smart->newMovingRange(
      KTextEditor::Range(cursor, cursor.line(), cursor.column() + 1));
    range->setAttribute(m_attr);

    if (m_cursors.empty()) {
      m_cursors.emplace_back(range);
      actionStartCurses();
    }
    else {
      m_cursors.emplace(it, range);
    }
  }
}

void MultiCursorView::setCursor()
{
	if (m_view->selection()) {
		const KTextEditor::Range& range = m_view->selectionRange();
		for (int line = range.start().line(); line != range.end().line() + 1; ++line) {
			setCursor(KTextEditor::Cursor(line, std::min(range.start().column(), m_document->lineLength(line))));
		}
	} else {
		setCursor(m_view->cursorPosition());
	}
}

void MultiCursorView::setActiveCtrlClick(bool active, bool remove_cursor_if_only_click)
{
    m_remove_cursor_if_only_click = remove_cursor_if_only_click;
    if (active) {
      m_view->focusProxy()->installEventFilter(this);
    }
    else {
      m_view->focusProxy()->removeEventFilter(this);
    }
}

bool MultiCursorView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            setCursor(m_view->cursorPosition());
            return false;
        }
        else if (m_remove_cursor_if_only_click) {
            removeAll();
            return false;
        }
    }
    return QObject::eventFilter(obj, event);
}

void MultiCursorView::textInserted(KTextEditor::Document *doc, const KTextEditor::Range &range)
{
	if (startEditing()) {
		insertText(doc->text(range));
		endEditing();
	}
}

void MultiCursorView::insertText(const QString &text)
{
  auto it = std::lower_bound(
    m_cursors.begin(), m_cursors.end(), m_view->cursorPosition());
  for (auto first = m_cursors.begin(); first != it; ++first) {
    m_document->insertText(first->cursor(), text);
    first->revalid();
  }
  auto last = m_cursors.end();
  if (it != last) {
    if (m_view->cursorPosition() != it->cursor()) {
      m_document->insertText(it->cursor(), text);
      it->revalid();
    }
    while (++it != last) {
      m_document->insertText(it->cursor(), text);
      it->revalid();
    }
  }
}

/*void MultiCursorView::debug() const
{
	for (Ranges::const_iterator it = m_cursors.begin(), end = m_cursors.end(); it != end; ++it) {
		qDebug() << **it;
	}
}*/

void MultiCursorView::textRemoved(
  KTextEditor::Document* doc, const KTextEditor::Range& range,
  const QString& text)
{
  Q_UNUSED(doc);
  Q_UNUSED(range);
  if (startEditing()) {
    CursorListDetail::removeBackwardText(
      m_document, m_cursors, text.length(),
      [this](Cursor const & c) {
        return c.cursor() != m_view->cursorPosition();
      }
    );
    endEditing();
  }
}

void MultiCursorView::removeRange(const CursorList::iterator &it)
{
	m_cursors.erase(it);
	if (m_cursors.empty())
		actionEmptyCurses();
}

void MultiCursorView::removeAll()
{
  if (m_view->selection()) {
    const KTextEditor::Range& range = m_view->selectionRange();
    auto first = std::lower_bound(
      m_cursors.begin(), m_cursors.end(), range.start());
    auto last = std::upper_bound(first, m_cursors.end(), range.start());
    m_cursors.erase(first, last);
    if (m_cursors.empty()) {
      actionEmptyCurses();
    }
  } else {
    m_cursors.clear();
    actionEmptyCurses();
  }
}

void MultiCursorView::removeLine()
{
  const int line = m_view->cursorPosition().line();
  auto first = std::lower_bound(
    m_cursors.begin(), m_cursors.end(), line
  , [](Cursor const & c, int line) { return c.line() != line; });
  auto last = std::upper_bound(
    first, m_cursors.end(), line
  , [](int line, Cursor const & c) { return c.line() != line; });
  m_cursors.erase(first, last);
  if (m_cursors.empty()) {
    actionEmptyCurses();
  }
}

void MultiCursorView::moveNext()
{
  auto it = std::lower_bound(
    m_cursors.begin(), m_cursors.end(), m_view->cursorPosition()
  , [](Cursor const & c1, KTextEditor::Cursor const & c2) {
    return c1.cursor() <= c2;
  });
  m_view->setCursorPosition(
    it != m_cursors.end() ? it->cursor() : m_cursors.front().cursor());
}

void MultiCursorView::movePrev()
{
  auto it = std::lower_bound(
    m_cursors.begin(), m_cursors.end(), m_view->cursorPosition());
  m_view->setCursorPosition(
    it != m_cursors.begin() ? (--it)->cursor() : m_cursors.back().cursor());
}

void MultiCursorView::setActive()
{
	if (m_active) {
		m_active = false;
		disconnectCurses();
	} else {
		m_active = true;
		connectCurses();
	}
}

bool MultiCursorView::startEditing()
{
	if (!m_active || m_text_edit || !m_document->startEditing())
		return false;
	if (m_synchronize)
		actionCollection()->action("synchronise_multicursor")->trigger();
	return m_text_edit = true;
}

bool MultiCursorView::endEditing()
{
	m_text_edit = false;
	return m_document->endEditing();
}

#include "multicursor.moc"
