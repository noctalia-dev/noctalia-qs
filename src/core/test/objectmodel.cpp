#include "objectmodel.hpp"

#include <qlist.h>
#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>

#include "../model.hpp"

void TestObjectModel::diffUpdateInsertRemove() {
	QObject a, b, c, d;

	auto model = ObjectModel<QObject>(nullptr);
	model.insertObject(&a);
	model.insertObject(&b);
	model.insertObject(&c);
	QCOMPARE(model.valueList(), (QList<QObject*> {&a, &b, &c}));

	// drop b, append d — relative order of survivors unchanged
	model.diffUpdate({&a, &c, &d});
	QCOMPARE(model.valueList(), (QList<QObject*> {&a, &c, &d}));
}

void TestObjectModel::diffUpdateReorder() {
	QObject a, b, c, d;

	auto model = ObjectModel<QObject>(nullptr);
	model.insertObject(&d);
	model.insertObject(&a);
	model.insertObject(&b);
	model.insertObject(&c);
	QCOMPARE(model.valueList(), (QList<QObject*> {&d, &a, &b, &c}));

	// pure permutation: same elements, different order. Regression for the
	// niri workspace duplication where re-sorting after an output migration
	// turned [d,a,b,c] into [a,b,c,d,a,b,c] because the old position was
	// never removed before inserting at the new one.
	model.diffUpdate({&a, &b, &c, &d});
	QCOMPARE(model.valueList(), (QList<QObject*> {&a, &b, &c, &d}));
}

QTEST_MAIN(TestObjectModel);
