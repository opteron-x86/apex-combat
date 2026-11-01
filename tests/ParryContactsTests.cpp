#include "core/ParryContacts.h"
#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main()
{
    Apex::Core::ParryContacts contacts;
    check(!contacts.contains(1, 10), "An ordinary contact has no recorded parry");
    check(!contacts.remember(0, 10), "Unidentified strikes cannot grant immunity");
    check(!contacts.remember(1, 0), "Invalid defenders cannot be recorded");
    check(contacts.remember(1, 10), "First successful parry is recorded");
    check(contacts.contains(1, 10), "Additional colliders from the same strike are recognized");
    check(!contacts.remember(1, 10), "Duplicate contacts must not refund twice");
    check(!contacts.contains(1, 20), "A different target is independent");
    check(contacts.remember(1, 20), "A sweep may be parried by a second defender");
    check(contacts.contains(1, 10), "The first defender remains protected during the strike");
    check(!contacts.contains(2, 10), "The next combo strike is not swallowed by a cooldown");
    check(contacts.remember(2, 10), "The next strike can grant a new reward immediately");
    check(!contacts.contains(1, 10), "A stale strike cannot match the current ledger");
    check(!contacts.remember(1, 20), "Out-of-order events cannot roll the ledger backward");
    check(contacts.contains(2, 10), "Rejecting a stale event preserves current immunity");
    contacts = {};
    check(contacts.remember(1, 10), "Actor/session reset restarts the sequence");
    std::cout << "Parry contact tests passed\n";
}
