**The Core:** Define the problem and its limits before coding.

* **The Happy Path:** Map the ideal success flow.
* **Edge Cases:** Map what happens when inputs are empty, negative, or missing.
* **The Why:** Without boundaries, "Scope Creep" creates bloated, dysfunctional software.

**Checklist:**

* [ ] Purpose stated in one simple sentence.
* [ ] At least five "What if?" scenarios identified.
* [ ] Explicit list of what the program *won't* do.

---

## 2. Structural Design

**The Core:** Organize code into logical, independent modules.

* **Cohesion & Coupling:** Keep related logic together (High Cohesion) but keep modules independent (Low Coupling).
* **Single Responsibility (SRP):** Each function or class must do exactly **one thing**.
* **The Why:** Over-connected code creates "Spaghetti Logic" where one small fix breaks the entire system.

**Checklist:**

* [ ] Function names describe exactly what they do.
* [ ] Parts can be changed without editing unrelated files.
* [ ] Logic is grouped into distinct, separate modules.

---

## 3. Data Integrity

**The Core:** Design data structures that accurately model reality.

* **Model Reality:** Use appropriate structures (lists, records) rather than "hacking" strings.
* **Encapsulation:** Hide complex internal logic; only expose necessary controls.
* **The Why:** Poor modeling creates "Technical Debt," forcing you to write complex code to fix simple data errors.

**Checklist:**

* [ ] Data stored in smallest logical format.
* [ ] Related data grouped into clear structures.
* [ ] Every piece of info has a "Single Source of Truth."

---

## 4. Defensive Programming

**The Core:** Assume the world is out to break your program.

* **Input Validation:** Validate everything entering the system.
* **Fail Gracefully:** Provide human-readable errors and shut down or recover safely.
* **The Why:** Without validation, a single user typo can cause a catastrophic system crash.

**Checklist:**

* [ ] All external inputs are checked before processing.
* [ ] Error messages are helpful to humans.
* [ ] System saves state before exiting on major errors.

---

## 5. Verification & Refinement

**The Core:** Prove it works, then make it simpler.

* **Testing:** Prove `Input A` always produces `Output B` via automated scripts.
* **Refactoring:** Shorten and clarify code without changing its behavior (Remove duplication).
* **The Why:** Without verification, you are just guessing. Without refinement, code becomes unreadable and unmaintainable.

**Checklist:**

* [ ] Every core logic function has a verification test.
* [ ] Code follows "DRY" (Don't Repeat Yourself) principles.
* [ ] Logic is clear enough for a stranger to understand.
